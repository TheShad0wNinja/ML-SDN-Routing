"""
Online FDRL local-agent ZMQ service.

Architecture
------------
- Main thread: ZMQ REP socket; cheap path. For each `observe` request it
  enqueues the observation onto a worker queue and immediately replies with
  the most recently produced action (or zeros on the very first tick). This
  bounds the ns-3 sim's per-tick stall to a few microseconds even when the
  agent is mid-training step.
- Training thread: consumes observations, stores transitions, runs a DDPG
  update, and writes the next action into a shared cache.

Wire protocol (JSON over ZMQ REQ/REP)
-------------------------------------
- `{"cmd":"hello", "arch":"gnn-v4", "num_switches":N, "num_links":L,
    "node_feat_dim":2, "edge_feat_dim":3, "link_action_dim":L,
    "node_action_dim":N, "action_dim":L+N, "seed":S,
    "resume":bool, "checkpoint_every_n_ticks":K}` → `{"ok":true}`
- `{"cmd":"observe", "tick":t, "state":{...}, "prev_reward":r,
    "explore":bool}` → `{"action":[float * (L+N)]}`
    The action vector is [edge cost-deltas (L) ‖ node sleep-values (N)]; C++
    splits on link_action_dim / node_action_dim.
- `{"cmd":"get_weights"}` → `{"weights":{name: list, ...}}`   (federation hook)
- `{"cmd":"set_weights", "weights":{...}}` → `{"ok":true}`     (federation hook)

The state dict from C++ carries `node_index` (frozen dpid list), `per_switch`
and `per_link` arrays in canonical order, and `residual_energy_stddev`. Python
turns it into a `torch_geometric.data.Data` and feeds a GAT actor/critic — so
weight shapes depend only on feature dims, not on the topology size. That is
the property federated averaging will rely on.

If torch or torch_geometric isn't installed, the service runs in degraded
mode and returns tiny random actions — useful for protocol-only smoke tests.
"""

from __future__ import annotations

import csv
import json
import math
import os
import pickle
import queue
import random
import re
import signal
import threading
import time
from collections import deque
from typing import Any

import zmq

try:
    import numpy as np
    import torch
    import torch.nn as nn
    import torch.nn.functional as F
    from torch_geometric.data import Data, Batch
    from torch_geometric.nn import GATv2Conv

    _HAS_TORCH = True
    torch.set_num_threads(1)
except Exception as exc:  # pragma: no cover — fires without torch or PyG
    _HAS_TORCH = False
    _TORCH_IMPORT_ERROR = exc


# Per-worker agent state. Set NS3_ML_AGENT_DIR to isolate parallel workers.
_AGENT_DIR = os.environ.get("NS3_ML_AGENT_DIR", "scratch/data/agent")
_CKPT_PATH = os.path.join(_AGENT_DIR, "local.pt")
_REPLAY_PATH = os.path.join(_AGENT_DIR, "replay.pkl")
_METRICS_PATH = os.path.join(_AGENT_DIR, "metrics.csv")

# Federation. When NS3_FEDAVG_DIR is set and NS3_FEDAVG_EVERY_STEPS > 0, the
# worker dumps its (actor, critic) state dicts to a shared directory every K
# train steps and blocks until the root aggregator publishes the averaged
# model. NS3_WORKER_ID identifies this worker in the shared dir (must be
# unique across the M participating workers).
_FEDAVG_DIR = os.environ.get("NS3_FEDAVG_DIR", "")
_FEDAVG_EVERY = int(os.environ.get("NS3_FEDAVG_EVERY_STEPS", "0") or 0)
_WORKER_ID = int(os.environ.get("NS3_WORKER_ID", "0"))
_FEDAVG_WAIT_TIMEOUT_S = float(
    os.environ.get("NS3_FEDAVG_WAIT_TIMEOUT_S", "300") or 300)

# Bumped when the on-disk format changes in a way the loader can't fix up.
# v3: dropped Oracle queue_depth — edge features shrank from 5→3
# (drop_norm, util, cost_norm). drop_norm is now normalized dynamically
# against per-link capacity, not against magic constants, so the GNN is
# scale-invariant across heterogeneous topologies and the trained model
# is deployable on stock OF1.3 hardware. Old checkpoints are incompatible.
_ARCH_TAG = "gnn-v4"

# Log-scale cap for unbounded RTT telemetry. log1p(1000) ≈ 6.91 — well past
# typical rtt_ms (~1s pathological).
_MAX_LOG_RTT = math.log1p(1000.0)

# GNN feature dims advertised by the C++ controller. Hardcoded here as defaults
# so the agent can still be instantiated for smoke tests; the real values come
# from the hello payload.
_NODE_FEAT_DIM = 2
_EDGE_FEAT_DIM = 3


# ---------------------------------------------------------------------------
# DDPG networks — GAT-based actor/critic. Weights depend only on feature
# dims (not N or L), which is the federated-averaging prerequisite.
# ---------------------------------------------------------------------------
if _HAS_TORCH:

    class _Actor(nn.Module):
        """Two-layer GATv2 encoder + per-edge head + per-node head.

        Two action streams share the GAT encoder:
          * edge stream — one action per canonical edge [E_total]: the per-link
            cost delta (the original action).
          * node stream — one action per node [N_total]: the per-switch sleep
            propensity. C++ thresholds tanh(node) to power switches off.

        forward() returns (edge_action, edge_logits, node_action, node_logits).
        action = tanh(logits); the raw pre-tanh logits are exposed so the
        saturation regulariser in train_step can penalise their magnitude (once
        tanh saturates to +/-1 its derivative ~0 and no signal can pull it back).
        For the wire the caller concatenates [edge_action ‖ node_action], which
        matches the action_dim = L + N the C++ side expects.
        """

        def __init__(self, node_dim: int = _NODE_FEAT_DIM,
                     edge_dim: int = _EDGE_FEAT_DIM,
                     hidden: int = 128, heads: int = 4):
            super().__init__()
            assert hidden % heads == 0, "hidden must be divisible by heads"
            self.gat1 = GATv2Conv(node_dim, hidden // heads, heads=heads,
                                  edge_dim=edge_dim)
            self.gat2 = GATv2Conv(hidden, hidden // heads, heads=heads,
                                  edge_dim=edge_dim)
            self.ln1 = nn.LayerNorm(hidden)
            self.ln2 = nn.LayerNorm(hidden)
            # NB: no nn.Tanh() here — applied explicitly in forward() so the
            # pre-tanh logits are recoverable.
            self.head = nn.Sequential(
                nn.Linear(2 * hidden + edge_dim, hidden),
                nn.ReLU(),
                nn.Linear(hidden, 1),
            )
            # Per-node sleep head off the same encoder embeddings.
            self.node_head = nn.Sequential(
                nn.Linear(hidden, hidden),
                nn.ReLU(),
                nn.Linear(hidden, 1),
            )

        def forward(self, data):
            h = F.relu(self.ln1(self.gat1(data.x, data.edge_index, data.edge_attr)))
            h = F.relu(self.ln2(self.gat2(h,      data.edge_index, data.edge_attr)))
            src, dst = data.canonical_edge_index  # each [E_total]
            feats = torch.cat([h[src], h[dst], data.canonical_edge_attr], dim=-1)
            edge_logits = self.head(feats).squeeze(-1)
            node_logits = self.node_head(h).squeeze(-1)  # [N_total]
            return (torch.tanh(edge_logits), edge_logits,
                    torch.tanh(node_logits), node_logits)

    class _Critic(nn.Module):
        """Two-layer GATv2 encoder + per-edge MLP + mean-edge pooling.

        Pooling is mean (not sum) so Q magnitude doesn't drift with topology
        size — that matters when weights are later averaged across controllers
        on different graphs.
        """

        def __init__(self, node_dim: int = _NODE_FEAT_DIM,
                     edge_dim: int = _EDGE_FEAT_DIM,
                     hidden: int = 128, heads: int = 4):
            super().__init__()
            assert hidden % heads == 0
            self.gat1 = GATv2Conv(node_dim, hidden // heads, heads=heads,
                                  edge_dim=edge_dim)
            self.gat2 = GATv2Conv(hidden, hidden // heads, heads=heads,
                                  edge_dim=edge_dim)
            self.ln1 = nn.LayerNorm(hidden)
            self.ln2 = nn.LayerNorm(hidden)
            # per-edge Q contribution: [h_src ‖ h_dst ‖ edge_attr ‖ edge_action]
            self.edge_head = nn.Sequential(
                nn.Linear(2 * hidden + edge_dim + 1, hidden),
                nn.ReLU(),
                nn.Linear(hidden, hidden),
            )
            # per-node Q contribution: [h_node ‖ node_action]
            self.node_head = nn.Sequential(
                nn.Linear(hidden + 1, hidden),
                nn.ReLU(),
                nn.Linear(hidden, hidden),
            )
            # global head: [pooled_edges ‖ pooled_nodes ‖ u] → scalar Q
            self.global_head = nn.Sequential(
                nn.Linear(2 * hidden + 1, hidden),
                nn.ReLU(),
                nn.Linear(hidden, 1),
            )

        def forward(self, data, edge_action: torch.Tensor,
                    node_action: torch.Tensor) -> torch.Tensor:
            h = F.relu(self.ln1(self.gat1(data.x, data.edge_index, data.edge_attr)))
            h = F.relu(self.ln2(self.gat2(h,      data.edge_index, data.edge_attr)))
            src, dst = data.canonical_edge_index
            feats = torch.cat([h[src], h[dst], data.canonical_edge_attr,
                               edge_action.unsqueeze(-1)], dim=-1)
            edge_repr = self.edge_head(feats)  # [E_total, hidden]
            node_repr = self.node_head(
                torch.cat([h, node_action.unsqueeze(-1)], dim=-1))  # [N_total, hidden]

            # Mean-pool edges and nodes per graph. For an unbatched single-graph
            # call data.batch is None, so we pool the whole thing.
            if getattr(data, "batch", None) is not None:
                # Map each canonical edge to its graph via its source node.
                edge_to_graph = data.batch[src]  # [E_total]
                num_graphs = int(data.batch.max().item()) + 1
                pooled_e = torch.zeros(num_graphs, edge_repr.size(1),
                                       device=edge_repr.device, dtype=edge_repr.dtype)
                pooled_e.index_add_(0, edge_to_graph, edge_repr)
                ecounts = torch.zeros(num_graphs, device=edge_repr.device,
                                      dtype=edge_repr.dtype)
                ecounts.index_add_(
                    0, edge_to_graph,
                    torch.ones_like(edge_to_graph, dtype=edge_repr.dtype))
                pooled_e = pooled_e / ecounts.clamp_min(1.0).unsqueeze(-1)
                # Nodes map to graphs directly via data.batch.
                pooled_n = torch.zeros(num_graphs, node_repr.size(1),
                                       device=node_repr.device, dtype=node_repr.dtype)
                pooled_n.index_add_(0, data.batch, node_repr)
                ncounts = torch.zeros(num_graphs, device=node_repr.device,
                                      dtype=node_repr.dtype)
                ncounts.index_add_(
                    0, data.batch,
                    torch.ones_like(data.batch, dtype=node_repr.dtype))
                pooled_n = pooled_n / ncounts.clamp_min(1.0).unsqueeze(-1)
                u = data.u.view(num_graphs, 1)
            else:
                pooled_e = edge_repr.mean(dim=0, keepdim=True)  # [1, hidden]
                pooled_n = node_repr.mean(dim=0, keepdim=True)  # [1, hidden]
                u = data.u.view(1, 1)

            return self.global_head(
                torch.cat([pooled_e, pooled_n, u], dim=-1)).squeeze(-1)


class LocalDDPGAgent:
    """Single-agent DDPG with decaying Gaussian exploration noise."""

    def __init__(
        self,
        action_dim: int,
        node_dim: int = _NODE_FEAT_DIM,
        edge_dim: int = _EDGE_FEAT_DIM,
        seed: int = 0,
        actor_lr: float = 1e-4,
        critic_lr: float = 1e-3,
        gamma: float = 0.99,
        tau: float = 0.005,
        replay_capacity: int = 200_000,
        batch_size: int = 64,
        warmup: int = 100,
        noise_sigma_init: float = 0.3,
        noise_sigma_min: float = 0.10,
        force_noise_sigma: bool = False,
        action_var_weight: float = 0.05,
        saturation_weight: float = 0.001,
    ):
        if not _HAS_TORCH:
            raise RuntimeError(
                f"LocalDDPGAgent requires torch + torch_geometric; import failed: "
                f"{_TORCH_IMPORT_ERROR}"
            )

        self.action_dim = action_dim
        self.node_dim = node_dim
        self.edge_dim = edge_dim
        self.gamma = gamma
        self.tau = tau
        self.batch_size = batch_size
        self.warmup = warmup
        # Anti-collapse regulariser weights. Default to the values C++
        # passes through HELLO; both can be 0 to disable.
        self._var_weight = float(action_var_weight)
        self._sat_weight = float(saturation_weight)
        # Counter for periodic action-stats logging. Not persisted.
        self._train_steps = 0

        torch.manual_seed(seed)
        np.random.seed(seed)
        random.seed(seed)

        self.actor = _Actor(node_dim=node_dim, edge_dim=edge_dim)
        self.critic = _Critic(node_dim=node_dim, edge_dim=edge_dim)
        self.actor_target = _Actor(node_dim=node_dim, edge_dim=edge_dim)
        self.critic_target = _Critic(node_dim=node_dim, edge_dim=edge_dim)
        self.actor_target.load_state_dict(self.actor.state_dict())
        self.critic_target.load_state_dict(self.critic.state_dict())
        self.actor_opt = torch.optim.Adam(self.actor.parameters(), lr=actor_lr)
        self.critic_opt = torch.optim.Adam(self.critic.parameters(), lr=critic_lr)

        self.replay: deque = deque(maxlen=replay_capacity)

        # Decaying Gaussian exploration noise. Sigma starts wide and decays
        # per gradient step (so slow scenarios still get the full schedule).
        # `noise_sigma_init` / `noise_sigma_min` (constructor kwargs, wired
        # from the C++ MlOptions flag chain via the hello payload) let a
        # cooldown / fine-tune phase pin sigma low without editing code.
        # _noise_sigma_init is stashed so reset_actor() can restart noise
        # from the configured value, not whatever the schedule had decayed to.
        self._noise_sigma_init = float(noise_sigma_init)
        self._noise_sigma = float(noise_sigma_init)
        self._noise_sigma_min = float(noise_sigma_min)
        self._noise_sigma_decay = 0.99995
        # When the controller passed an explicit sigma (via --mlNoiseSigma),
        # the checkpoint's saved sigma must not override it on resume.
        self._force_noise_sigma = bool(force_noise_sigma)

    # ------------------------------------------------------------------
    # Replay + training
    # ------------------------------------------------------------------
    def push(self, s, a, r, s_next):
        """Store one (s, a, r, s') transition.

        s and s_next are torch_geometric Data objects; a is a numpy array of
        shape [action_dim].
        """
        self.replay.append((s, np.asarray(a, dtype=np.float32),
                            float(r), s_next))

    def _sample(self):
        idx = np.random.randint(0, len(self.replay), self.batch_size)
        batch = [self.replay[i] for i in idx]
        s, a, r, s2 = zip(*batch)
        s_batch = Batch.from_data_list(list(s))
        s2_batch = Batch.from_data_list(list(s2))
        # The stored action is the flat wire vector [edge_deltas (L) ‖ node (N)].
        # PyG's Batch concatenates edges and nodes separately (each in list
        # order), so we split each sample's action into its edge part and node
        # part and concatenate them in the same per-stream order. N is the node
        # count of the state graph; the node part is the last N entries.
        edge_parts, node_parts = [], []
        for si, ai in zip(s, a):
            n_i = int(si.x.shape[0])
            if n_i > 0:
                edge_parts.append(ai[:-n_i])
                node_parts.append(ai[-n_i:])
            else:
                edge_parts.append(ai)
                node_parts.append(ai[:0])
        edge_a = torch.from_numpy(np.concatenate(edge_parts, axis=0))
        node_a = torch.from_numpy(np.concatenate(node_parts, axis=0))
        r_tensor = torch.tensor(r, dtype=torch.float32).unsqueeze(-1)
        return s_batch, edge_a, node_a, r_tensor, s2_batch

    def train_step(self):
        if len(self.replay) < max(self.warmup, self.batch_size):
            return None, None

        s, edge_a, node_a, r, s2 = self._sample()
        with torch.no_grad():
            e2, _, n2, _ = self.actor_target(s2)
            q_target = r + self.gamma * self.critic_target(s2, e2, n2).unsqueeze(-1)
        q = self.critic(s, edge_a, node_a).unsqueeze(-1)
        critic_loss = F.mse_loss(q, q_target)
        self.critic_opt.zero_grad()
        critic_loss.backward()
        torch.nn.utils.clip_grad_norm_(self.critic.parameters(), max_norm=1.0)
        self.critic_opt.step()

        # Actor loss = -E[Q(s, mu(s))] + anti-collapse regularisers:
        #   var_term:  reward spread across per-link outputs so a uniform
        #              constant policy strictly loses to a differentiated one.
        #   sat_term:  squared L2 on pre-tanh logits — pulls them toward 0
        #              (linear region of tanh) so gradient stays alive even
        #              after long training runs. Without this term, once the
        #              actor saturates to +/-1 the tanh derivative is ~0 and
        #              no further signal can pull it back.
        edge_act, edge_logits, node_act, node_logits = self.actor(s)
        q_loss = -self.critic(s, edge_act, node_act).mean()
        # Anti-collapse regularisers span both action streams.
        action = torch.cat([edge_act, node_act], dim=0)
        logits = torch.cat([edge_logits, node_logits], dim=0)
        var_term = -self._var_weight * action.var(dim=0).mean()
        sat_term = self._sat_weight * (logits ** 2).mean()
        actor_loss = q_loss + var_term + sat_term
        self.actor_opt.zero_grad()
        actor_loss.backward()
        torch.nn.utils.clip_grad_norm_(self.actor.parameters(), max_norm=1.0)
        self.actor_opt.step()

        # Polyak updates
        with torch.no_grad():
            for p, pt in zip(self.actor.parameters(), self.actor_target.parameters()):
                pt.data.mul_(1 - self.tau).add_(self.tau * p.data)
            for p, pt in zip(self.critic.parameters(), self.critic_target.parameters()):
                pt.data.mul_(1 - self.tau).add_(self.tau * p.data)

        self._noise_sigma = max(self._noise_sigma_min,
                                self._noise_sigma * self._noise_sigma_decay)

        # Periodic action-stats log: the abs_max of pre-tanh logits is the
        # smoking-gun signal for saturation — if it stays below ~2.5 the
        # actor is healthy; above ~4 it has collapsed again.
        self._train_steps += 1
        if self._train_steps % 50 == 0:
            with torch.no_grad():
                a_np = action.detach().cpu().numpy()
                l_np = logits.detach().cpu().numpy()
            print(f"[ML-PY] step={self._train_steps} "
                  f"action(mean={a_np.mean():+.3f} std={a_np.std():.3f} "
                  f"min={a_np.min():+.3f} max={a_np.max():+.3f}) "
                  f"logits(abs_max={abs(l_np).max():.2f}) "
                  f"q_loss={float(q_loss.item()):+.3f} "
                  f"var_term={float(var_term.item()):+.4f} "
                  f"sat_term={float(sat_term.item()):+.4f} "
                  f"sigma={self._noise_sigma:.3f}", flush=True)

        return float(critic_loss.item()), float(actor_loss.item())

    # ------------------------------------------------------------------
    # Acting
    # ------------------------------------------------------------------
    def act(self, data: "Data", explore: bool = True) -> "np.ndarray":
        """Return the flat wire action [edge_deltas (L) ‖ node_sleep (N)] for a
        single graph — matching the action_dim = L + N the C++ side expects."""
        with torch.no_grad():
            # Wrap into a batch-of-one so downstream code can stay uniform.
            batch = Batch.from_data_list([data])
            edge_act, _, node_act, _ = self.actor(batch)
            a = torch.cat([edge_act, node_act], dim=0).cpu().numpy().astype(
                np.float32)

        if explore:
            noise = np.random.normal(0.0, self._noise_sigma,
                                     size=a.shape).astype(np.float32)
            a = np.clip(a + noise, -1.0, 1.0)
        return a

    # ------------------------------------------------------------------
    # Persistence — federation hooks live here too.
    # ------------------------------------------------------------------
    def export_weights(self) -> dict:
        return {
            "actor": {k: v.cpu().numpy().tolist()
                      for k, v in self.actor.state_dict().items()},
            "critic": {k: v.cpu().numpy().tolist()
                       for k, v in self.critic.state_dict().items()},
        }

    def load_weights(self, payload: dict) -> None:
        if "actor" in payload:
            self.actor.load_state_dict(
                {k: torch.tensor(v) for k, v in payload["actor"].items()})
            self.actor_target.load_state_dict(self.actor.state_dict())
        if "critic" in payload:
            self.critic.load_state_dict(
                {k: torch.tensor(v) for k, v in payload["critic"].items()})
            self.critic_target.load_state_dict(self.critic.state_dict())

    def reset_actor(self, actor_lr: float = 1e-4) -> None:
        """Fresh-init the actor (+ actor_target + actor_opt) without touching
        the critic, critic_target, critic_opt, or replay buffer. Used to
        salvage a policy that has collapsed to a constant: the value function
        learned by the critic and the experience in replay both stay valid;
        only the actor's broken parameter manifold gets thrown away.

        Adam state is bound to specific parameter tensors, so the optimiser
        MUST be re-created against the new actor.parameters() — calling
        load_state_dict on the old one would point at freed memory.
        """
        seed = int(np.random.randint(0, 2**31 - 1))
        torch.manual_seed(seed)
        self.actor = _Actor(node_dim=self.node_dim, edge_dim=self.edge_dim)
        self.actor_target = _Actor(node_dim=self.node_dim, edge_dim=self.edge_dim)
        self.actor_target.load_state_dict(self.actor.state_dict())
        self.actor_opt = torch.optim.Adam(self.actor.parameters(), lr=actor_lr)
        # Restart noise from the configured init so the fresh actor gets full
        # exploration. force_noise_sigma is intentionally NOT touched — if
        # the user pinned sigma via --mlNoiseSigma they want it to remain
        # pinned through the recovery run too.
        self._noise_sigma = float(self._noise_sigma_init)
        self._train_steps = 0
        print(f"[ML] actor reset: fresh weights, critic + replay preserved "
              f"(replay size={len(self.replay)}, sigma={self._noise_sigma:.3f})",
              flush=True)

    def save_checkpoint(self, path: str = _CKPT_PATH,
                        replay_path: str = _REPLAY_PATH) -> None:
        os.makedirs(os.path.dirname(path), exist_ok=True)
        tmp = path + ".tmp"
        torch.save({
            "arch": _ARCH_TAG,
            "node_dim": self.node_dim,
            "edge_dim": self.edge_dim,
            "action_dim": self.action_dim,
            "actor": self.actor.state_dict(),
            "critic": self.critic.state_dict(),
            "actor_target": self.actor_target.state_dict(),
            "critic_target": self.critic_target.state_dict(),
            "actor_opt": self.actor_opt.state_dict(),
            "critic_opt": self.critic_opt.state_dict(),
            "noise_sigma": float(self._noise_sigma),
        }, tmp)
        os.replace(tmp, path)

        tmp_r = replay_path + ".tmp"
        with open(tmp_r, "wb") as f:
            pickle.dump({"arch": _ARCH_TAG, "items": list(self.replay)}, f,
                        protocol=pickle.HIGHEST_PROTOCOL)
        os.replace(tmp_r, replay_path)

    def maybe_load_checkpoint(self, path: str = _CKPT_PATH,
                              replay_path: str = _REPLAY_PATH) -> bool:
        if not os.path.exists(path):
            return False
        try:
            blob = torch.load(path, map_location="cpu")
            arch = blob.get("arch", "mlp-legacy")
            if arch != _ARCH_TAG:
                print(f"[ML] checkpoint arch={arch!r} != expected={_ARCH_TAG!r} — "
                      f"refusing to load (likely a pre-GNN checkpoint). Starting fresh.")
                return False
            self.actor.load_state_dict(blob["actor"])
            self.critic.load_state_dict(blob["critic"])
            self.actor_target.load_state_dict(blob["actor_target"])
            self.critic_target.load_state_dict(blob["critic_target"])
            self.actor_opt.load_state_dict(blob["actor_opt"])
            self.critic_opt.load_state_dict(blob["critic_opt"])
            if "noise_sigma" in blob and not self._force_noise_sigma:
                self._noise_sigma = float(blob["noise_sigma"])
        except Exception as exc:
            print(f"[ML] checkpoint load failed: {exc}")
            return False
        if os.path.exists(replay_path):
            try:
                with open(replay_path, "rb") as f:
                    payload = pickle.load(f)
                if isinstance(payload, dict) and payload.get("arch") == _ARCH_TAG:
                    self.replay = deque(payload["items"],
                                        maxlen=self.replay.maxlen)
                else:
                    print(f"[ML] replay buffer arch mismatch — discarding.")
            except Exception as exc:
                print(f"[ML] replay load failed: {exc}")
        return True


# ---------------------------------------------------------------------------
# Service
# ---------------------------------------------------------------------------
def _build_graph_data(state: dict) -> "Data":
    """Build a torch_geometric Data object from the controller's state JSON.

    Expected schema (from C++ BuildMlStatePayload, gnn-v3):
      state.node_index = [dpid_0, dpid_1, ...]   (frozen canonical order)
      state.per_switch = [{dpid, depletion, echo_rtt_ns}, ...]
      state.per_link   = [{src, dst, tx_bps, capacity_bps, utilization, cost,
                           base_cost, delay_ms,
                           src_tx_bps, src_utilization, src_drop_rate_bps,
                           dst_tx_bps, dst_utilization, dst_drop_rate_bps}, ...]
      state.residual_energy_stddev = float

    Returns Data with:
      x                    [N, 2]   node features: depletion, log1p(rtt_ms)/X
      edge_index           [2, 2L]  bidirectional message-passing edges
      edge_attr            [2L, 3]  features per direction (asymmetric):
                                     [drop_norm, utilization, cost_norm]
      canonical_edge_index [2, L]   one direction per link (matches action vec)
      canonical_edge_attr  [L, 3]   src-direction features for canonical edges
      u                    [1]      global residual_energy_stddev

    Drop rate is normalized against per-link capacity (not a magic constant),
    so the feature stays in [0, 1] across heterogeneous topologies — a 1 Mbps
    drop on a 10 Gbps core link reads ~0.0001 while the same drop on a 10 Mbps
    edge link reads 0.1. The GNN learns capacity-relative congestion physics
    instead of memorizing raw byte counts.

    Traffic physics lives on edges, not nodes — congestion is a property of
    a port, not a switch. The reverse-direction message-passing edge uses
    `dst_*` fields so the GAT can attend asymmetrically (a saturated egress
    port on one side of a link doesn't imply backpressure on the other).
    """
    node_index = state.get("node_index", []) or []
    dpid_to_idx = {int(d): i for i, d in enumerate(node_index)}
    n = max(len(node_index), 1)

    x = np.zeros((n, _NODE_FEAT_DIM), dtype=np.float32)
    for sw in state.get("per_switch", []) or []:
        idx = dpid_to_idx.get(int(sw.get("dpid", -1)), -1)
        if idx < 0:
            continue
        rtt_ms = max(0.0, float(sw.get("echo_rtt_ns", 0.0))) / 1.0e6
        rtt_norm = math.log1p(rtt_ms) / _MAX_LOG_RTT
        x[idx, 0] = float(sw.get("depletion", 0.0))
        x[idx, 1] = float(rtt_norm)

    per_link = state.get("per_link", []) or []
    L = len(per_link)
    # Normalize cost by the max in this tick so the agent sees a unit-scale signal.
    max_cost = max((float(l.get("cost", 1.0)) for l in per_link), default=1.0)
    max_cost = max(max_cost, 1e-9)

    def edge_features(l: dict, drop_bps: float, util: float) -> np.ndarray:
        cap = float(l.get("capacity_bps", 0.0))
        safe_cap = cap if cap > 0.0 else 1.0   # avoid /0 on dead links
        return np.array([
            min(1.0, max(0.0, drop_bps) / safe_cap),
            float(util),
            float(l.get("cost", 0.0)) / max_cost,
        ], dtype=np.float32)

    canon_src = np.zeros(max(L, 1), dtype=np.int64)
    canon_dst = np.zeros(max(L, 1), dtype=np.int64)
    canon_attr = np.zeros((max(L, 1), _EDGE_FEAT_DIM), dtype=np.float32)
    reverse_attr = np.zeros((max(L, 1), _EDGE_FEAT_DIM), dtype=np.float32)
    for i, l in enumerate(per_link):
        canon_src[i] = dpid_to_idx.get(int(l.get("src", -1)), 0)
        canon_dst[i] = dpid_to_idx.get(int(l.get("dst", -1)), 0)
        canon_attr[i] = edge_features(
            l,
            float(l.get("src_drop_rate_bps", 0.0)),
            float(l.get("src_utilization", l.get("utilization", 0.0))))
        reverse_attr[i] = edge_features(
            l,
            float(l.get("dst_drop_rate_bps", 0.0)),
            float(l.get("dst_utilization", 0.0)))

    # When L=0 we still emit a dummy self-loop on node 0 so GATConv has something
    # to chew on. With no real edges the gradient through edge_attr is null
    # anyway, so this just prevents an empty-tensor crash on the first tick.
    if L == 0:
        canon_src[0] = 0
        canon_dst[0] = 0
        L_eff = 1
    else:
        L_eff = L

    canonical_edge_index = np.stack([canon_src[:L_eff], canon_dst[:L_eff]], axis=0)
    bidir_src = np.concatenate([canon_src[:L_eff], canon_dst[:L_eff]])
    bidir_dst = np.concatenate([canon_dst[:L_eff], canon_src[:L_eff]])
    bidir_edge_index = np.stack([bidir_src, bidir_dst], axis=0)
    # Canonical (src→dst) attrs first; reverse (dst→src) attrs second — matching
    # the bidir_src/bidir_dst concatenation order above.
    bidir_edge_attr = np.concatenate([canon_attr[:L_eff], reverse_attr[:L_eff]],
                                     axis=0)

    u = np.array([float(state.get("residual_energy_stddev", 0.0))],
                 dtype=np.float32)

    data = Data(
        x=torch.from_numpy(x),
        edge_index=torch.from_numpy(bidir_edge_index),
        edge_attr=torch.from_numpy(bidir_edge_attr),
        u=torch.from_numpy(u),
    )
    # PyG batching: any attribute whose name contains "_index" is auto-offset
    # by num_nodes when graphs are batched — exactly what we need so
    # canonical_edge_index keeps pointing at the right nodes after batching.
    data.canonical_edge_index = torch.from_numpy(canonical_edge_index)
    data.canonical_edge_attr = torch.from_numpy(canon_attr[:L_eff].copy())
    return data


class MLService:
    """Threaded REP-socket DDPG service."""

    def __init__(self, bind_endpoint: str = "tcp://*:5555"):
        self.ctx = zmq.Context()
        self.rep = self.ctx.socket(zmq.REP)
        self.rep.bind(bind_endpoint)

        # Agent state — populated by `hello`.
        self.agent: LocalDDPGAgent | None = None
        self.num_switches = 0
        self.num_links = 0
        self.node_feat_dim = _NODE_FEAT_DIM
        self.edge_feat_dim = _EDGE_FEAT_DIM
        self.action_dim = 0
        self.checkpoint_every = 60
        self.seed = 0

        # Worker thread coordination.
        # Tuple is (tick, graph_data, prev_reward, explore).
        self._req_q: "queue.Queue[tuple[int, Any, float, bool]]" = queue.Queue()
        self._stop = threading.Event()
        self._action_lock = threading.Lock()
        # Cached "best action so far". Producer = worker, consumer = main.
        self._last_action: "np.ndarray | None" = None
        self._prev_state: "Any | None" = None
        self._prev_action: "np.ndarray | None" = None
        self._train_steps = 0

        # Metrics CSV — opened lazily on first train step.
        self._metrics_fh = None
        self._metrics_writer = None

        # Federation round counter. Incremented after each fedavg submit+load
        # regardless of outcome — the aggregator is the source of truth for
        # round numbering, but workers need to track which round they're on
        # so a re-derived (train_steps // K) formula doesn't skip round 0.
        #
        # On resume, start from max(global_round_*) + 1 so we don't replay
        # historical averages (which would load progressively older weights
        # into the live agent and corrupt training). Matches the aggregator's
        # _detect_next_round in root_aggregator.py.
        self._fedavg_round = self._detect_resume_round()
        if self._fedavg_round > 0:
            print(f"[ML] fedavg resume: starting at round "
                  f"{self._fedavg_round} (latest global="
                  f"{self._fedavg_round - 1})")

    # ------------------------------------------------------------------
    def run(self) -> None:
        # Turn SIGTERM into a clean shutdown so the `finally` block below saves
        # the latest checkpoint. Without this, a `kill <pid>` from the test
        # runner would lose anything since the last periodic save.
        def _on_sigterm(_signum, _frame):
            raise KeyboardInterrupt
        signal.signal(signal.SIGTERM, _on_sigterm)

        print("ML Service listening on tcp://*:5555")
        worker = threading.Thread(target=self._worker_loop, daemon=True)
        worker.start()
        try:
            while True:
                data = self.rep.recv()
                response = self._process_request(data)
                self.rep.send(response)
        except KeyboardInterrupt:
            print("ML Service shutting down.")
        finally:
            self._stop.set()
            # Persist whatever the agent has learned since the last periodic
            # save. Cheap insurance against losing the tail of a long run.
            if self.agent is not None and _HAS_TORCH:
                try:
                    self.agent.save_checkpoint()
                    print("[ML] checkpoint saved on shutdown.")
                except Exception as exc:
                    print(f"[ML] shutdown checkpoint save failed: {exc}")
            try:
                self.rep.setsockopt(zmq.LINGER, 0)
                self.rep.close()
                self.ctx.destroy(linger=0)
            except Exception:
                pass
            if self._metrics_fh:
                self._metrics_fh.close()

    # ------------------------------------------------------------------
    # Worker thread — does the expensive work.
    # ------------------------------------------------------------------
    def _worker_loop(self) -> None:
        while not self._stop.is_set():
            try:
                tick, graph, prev_reward, explore, learn = self._req_q.get(timeout=0.5)
            except queue.Empty:
                continue
            if self.agent is None or not _HAS_TORCH:
                continue

            # 1. Store (s, a, r, s') if we have a prior action cached.
            if self._prev_state is not None and self._prev_action is not None:
                self.agent.push(self._prev_state, self._prev_action,
                                prev_reward, graph)

            # 2. Train step (no-op until replay has warmup samples).
            #    Gated on `learn` so eval mode (learn=false) keeps the policy
            #    frozen for deterministic ML-vs-baseline comparisons, while
            #    online fine-tuning (explore=false, learn=true) can keep
            #    updating weights without injecting action noise.
            t0 = time.perf_counter()
            if learn:
                critic_loss, actor_loss = self.agent.train_step()
            else:
                critic_loss, actor_loss = None, None
            train_ms = (time.perf_counter() - t0) * 1000.0

            # 3. Choose next action and publish.
            next_action = self.agent.act(graph, explore=explore)
            with self._action_lock:
                self._last_action = next_action

            self._prev_state = graph
            self._prev_action = next_action
            self._train_steps += 1

            # 4. Metrics
            self._log_metrics(
                tick=tick,
                reward=prev_reward,
                critic_loss=critic_loss,
                actor_loss=actor_loss,
                mean_abs_action=float(np.mean(np.abs(next_action))),
                replay_size=len(self.agent.replay),
                wall_clock_ms=train_ms,
            )

            # 5. Periodic checkpoint
            if (self.checkpoint_every > 0
                    and self._train_steps % self.checkpoint_every == 0):
                try:
                    self.agent.save_checkpoint()
                except Exception as exc:
                    print(f"[ML] checkpoint save failed: {exc}")

            # 6. Federation. Dump weights, block on the global average.
            #    The IO thread keeps serving `act()` with the last cached
            #    action during the wait, so ns-3 never stalls. The round
            #    counter advances unconditionally — if a worker hits the
            #    wait timeout, it still moves to the next round so it stays
            #    in lockstep with the aggregator.
            if (_FEDAVG_DIR and _FEDAVG_EVERY > 0
                    and self._train_steps % _FEDAVG_EVERY == 0):
                try:
                    self._do_fedavg_round()
                except Exception as exc:
                    print(f"[ML] fedavg round failed: {exc}")
                self._fedavg_round += 1

    # ------------------------------------------------------------------
    @staticmethod
    def _detect_resume_round() -> int:
        """Return the next fedavg round to participate in.

        Scans _FEDAVG_DIR for global_round_<N>.pt and returns max(N)+1, or
        0 when nothing exists yet. Without this, a resumed worker would start
        at round 0 and silently overwrite its weights with every old global
        average on disk on its way back up to the current round."""
        if not _FEDAVG_DIR:
            return 0
        try:
            entries = os.listdir(_FEDAVG_DIR)
        except FileNotFoundError:
            return 0
        rounds: list[int] = []
        pat = re.compile(r"^global_round_(\d+)\.pt$")
        for name in entries:
            m = pat.match(name)
            if m:
                rounds.append(int(m.group(1)))
        return max(rounds) + 1 if rounds else 0

    # ------------------------------------------------------------------
    def _do_fedavg_round(self) -> None:
        """Submit local weights to the shared dir, wait for the global model,
        and replace local actor/critic with the averaged version.

        Files written:  {_FEDAVG_DIR}/worker_{worker_id}_round_{r}.pt
        File polled:    {_FEDAVG_DIR}/global_round_{r}.pt
        """
        if self.agent is None or not _HAS_TORCH:
            return
        r = self._fedavg_round
        os.makedirs(_FEDAVG_DIR, exist_ok=True)
        worker_path = os.path.join(
            _FEDAVG_DIR, f"worker_{_WORKER_ID}_round_{r}.pt")
        global_path = os.path.join(_FEDAVG_DIR, f"global_round_{r}.pt")

        # 1. Snapshot — atomic write (.tmp then rename) so the aggregator
        #    never sees a partial file.
        t0 = time.perf_counter()
        blob = {
            "arch": _ARCH_TAG,
            "worker_id": _WORKER_ID,
            "round": r,
            "actor": self.agent.actor.state_dict(),
            "critic": self.agent.critic.state_dict(),
        }
        tmp = worker_path + ".tmp"
        torch.save(blob, tmp)
        os.replace(tmp, worker_path)

        # 2. Wait for the aggregator. Soft timeout: keep training with the
        #    pre-round local weights rather than hanging the sim.
        deadline = t0 + _FEDAVG_WAIT_TIMEOUT_S
        while not os.path.exists(global_path):
            if self._stop.is_set():
                return
            if time.perf_counter() > deadline:
                print(f"[ML] fedavg round {r} wait timed out after "
                      f"{_FEDAVG_WAIT_TIMEOUT_S:.0f}s — continuing without "
                      f"the global update.")
                return
            time.sleep(0.5)

        # 3. Load. Replace actor+critic and re-sync the target nets so the
        #    polyak update doesn't immediately undo the average.
        try:
            global_blob = torch.load(global_path, map_location="cpu")
        except Exception as exc:
            print(f"[ML] fedavg load failed for round {r}: {exc}")
            return
        if global_blob.get("arch") != _ARCH_TAG:
            print(f"[ML] fedavg round {r}: global arch="
                  f"{global_blob.get('arch')!r} != {_ARCH_TAG!r} — skipping.")
            return
        self.agent.actor.load_state_dict(global_blob["actor"])
        self.agent.actor_target.load_state_dict(self.agent.actor.state_dict())
        self.agent.critic.load_state_dict(global_blob["critic"])
        self.agent.critic_target.load_state_dict(
            self.agent.critic.state_dict())
        wait_ms = (time.perf_counter() - t0) * 1000.0
        print(f"[ML] fedavg round {r}: loaded global "
              f"(wait={wait_ms:.0f}ms, worker={_WORKER_ID})")

    # ------------------------------------------------------------------
    def _log_metrics(self, **row) -> None:
        os.makedirs(_AGENT_DIR, exist_ok=True)
        if self._metrics_fh is None:
            new_file = not os.path.exists(_METRICS_PATH)
            self._metrics_fh = open(_METRICS_PATH, "a", newline="")
            self._metrics_writer = csv.DictWriter(
                self._metrics_fh,
                fieldnames=["worker_id", "tick", "reward", "critic_loss",
                            "actor_loss", "mean_abs_action", "replay_size",
                            "wall_clock_ms"],
            )
            if new_file:
                self._metrics_writer.writeheader()
        row.setdefault("worker_id", _WORKER_ID)
        # Replace None with "" so CSV stays parseable.
        clean = {k: ("" if v is None else v) for k, v in row.items()}
        if self._metrics_writer is not None:
            self._metrics_writer.writerow(clean)
        self._metrics_fh.flush()

    # ------------------------------------------------------------------
    # REP dispatch — runs on the main thread, must stay fast.
    # ------------------------------------------------------------------
    def _process_request(self, data: bytes) -> bytes:
        if not data:
            return b""
        try:
            msg = json.loads(data.decode("utf-8"))
        except (json.JSONDecodeError, UnicodeDecodeError):
            return json.dumps({"error": "bad-json"}).encode("utf-8")

        cmd = msg.get("cmd")
        if cmd == "hello":
            return self._handle_hello(msg)
        if cmd == "observe":
            return self._handle_observe(msg)
        if cmd == "get_weights":
            return self._handle_get_weights()
        if cmd == "set_weights":
            return self._handle_set_weights(msg)
        return json.dumps({"error": f"unknown-cmd:{cmd}"}).encode("utf-8")

    def _handle_hello(self, msg: dict) -> bytes:
        arch = str(msg.get("arch", ""))
        self.num_switches = int(msg.get("num_switches", 0))
        self.num_links = int(msg.get("num_links", 0))
        self.node_feat_dim = int(msg.get("node_feat_dim", _NODE_FEAT_DIM))
        self.edge_feat_dim = int(msg.get("edge_feat_dim", _EDGE_FEAT_DIM))
        self.action_dim = int(msg.get("action_dim", self.num_links))
        self.checkpoint_every = int(msg.get("checkpoint_every_n_ticks", 60))
        self.seed = int(msg.get("seed", 0))
        resume = bool(msg.get("resume", True))
        # Sentinel: controller sends -1 when the user didn't pass --mlNoiseSigma,
        # in which case we fall back to the agent's default schedule.
        noise_init = float(msg.get("noise_sigma_init", -1.0))
        force_sigma = noise_init >= 0.0
        # Anti-collapse knobs. Defaults match LocalDDPGAgent's __init__ so an
        # older controller that doesn't send these fields gets the same
        # behaviour as a new one with default flags.
        noise_min = float(msg.get("noise_sigma_min", 0.10))
        var_weight = float(msg.get("action_var_weight", 0.05))
        sat_weight = float(msg.get("saturation_weight", 0.001))
        reset_actor_req = bool(msg.get("reset_actor", False))

        if not _HAS_TORCH:
            print(f"[ML] hello received but torch/torch_geometric missing — "
                  f"degraded mode ({_TORCH_IMPORT_ERROR})")
            return json.dumps({"ok": True, "degraded": True}).encode("utf-8")

        if arch and arch != _ARCH_TAG:
            print(f"[ML] WARNING: controller arch={arch!r} doesn't match "
                  f"service arch={_ARCH_TAG!r} — proceeding anyway.")

        try:
            agent_kwargs = dict(
                action_dim=self.action_dim,
                node_dim=self.node_feat_dim,
                edge_dim=self.edge_feat_dim,
                seed=self.seed,
                noise_sigma_min=noise_min,
                action_var_weight=var_weight,
                saturation_weight=sat_weight,
            )
            if force_sigma:
                agent_kwargs["noise_sigma_init"] = noise_init
                agent_kwargs["force_noise_sigma"] = True
                print(f"[ML] noise sigma pinned via --mlNoiseSigma: "
                      f"init={noise_init} (checkpoint sigma will be ignored)")
            self.agent = LocalDDPGAgent(**agent_kwargs)
        except Exception as exc:
            print(f"[ML] agent init failed: {exc}")
            self.agent = None
            return json.dumps({"ok": False, "error": str(exc)}).encode("utf-8")

        resumed = False
        if resume:
            try:
                resumed = self.agent.maybe_load_checkpoint()
            except Exception as exc:
                print(f"[ML] resume failed: {exc}")

        # Partial-reset request (--mlResetActor=true): fresh-init the actor
        # AFTER the checkpoint is loaded. Critic + replay survive. Use to
        # recover a collapsed policy without burning the 4-hour value
        # function. The flag is one-shot — the next save_checkpoint writes
        # a normal blob, so subsequent resumes get the fresh actor as-is.
        if reset_actor_req and resumed:
            try:
                self.agent.reset_actor()
            except Exception as exc:
                print(f"[ML] reset_actor failed: {exc}")
        elif reset_actor_req and not resumed:
            print("[ML] reset_actor requested but no checkpoint to resume "
                  "from — agent already has fresh weights, no-op.")

        # Seed the action cache so the very first observe gets a real reply.
        with self._action_lock:
            self._last_action = np.zeros(self.action_dim, dtype=np.float32)

        # FL property check: print weight shapes so the topology-invariance is
        # visible in logs. None of these tuples should contain N or L.
        actor_shapes = {k: tuple(v.shape)
                        for k, v in self.agent.actor.state_dict().items()}
        print(f"[ML] hello: arch={_ARCH_TAG} num_switches={self.num_switches} "
              f"num_links={self.num_links} node_feat_dim={self.node_feat_dim} "
              f"edge_feat_dim={self.edge_feat_dim} action_dim={self.action_dim} "
              f"seed={self.seed} resumed={resumed}")
        print(f"[ML] actor weight shapes (topology-invariant): {actor_shapes}")
        return json.dumps({"ok": True, "resumed": resumed,
                           "arch": _ARCH_TAG}).encode("utf-8")

    def _handle_observe(self, msg: dict) -> bytes:
        tick = int(msg.get("tick", 0))
        prev_reward = float(msg.get("prev_reward", 0.0))
        # Default explore=true so behaviour is unchanged for older controllers
        # that don't send the field.
        explore = bool(msg.get("explore", True))
        # `learn` gates train_step independently of action noise. Default to
        # `explore` so older controllers (which only send `explore`) keep the
        # original coupled semantics.
        learn = bool(msg.get("learn", explore))
        state = msg.get("state", {}) or {}

        if not _HAS_TORCH:
            # Degraded mode: tiny random action (zero if eval).
            n = self.action_dim if self.action_dim else len(state.get("per_link", []))
            jitter = 0.02 if explore else 0.0
            action = [random.uniform(-jitter, jitter) for _ in range(n)]
            return json.dumps({"action": action}).encode("utf-8")

        try:
            graph = _build_graph_data(state)
        except Exception as exc:
            print(f"[ML] failed to build graph from state: {exc}")
            return json.dumps(
                {"action": [0.0] * max(self.action_dim, 1)}
            ).encode("utf-8")

        # Enqueue to worker — never block for more than a microsecond.
        try:
            self._req_q.put_nowait((tick, graph, prev_reward, explore, learn))
        except queue.Full:
            pass

        with self._action_lock:
            action = (self._last_action
                      if self._last_action is not None
                      else np.zeros(self.action_dim, dtype=np.float32))
            action_list = action.tolist()
        return json.dumps({"action": action_list}).encode("utf-8")

    def _handle_get_weights(self) -> bytes:
        # Federation hook: returns weights if the agent exists, else empty.
        # Shapes are topology-invariant after the GNN refactor — see the log
        # at hello time for the actual tuples.
        if self.agent is None or not _HAS_TORCH:
            return json.dumps({"weights": {}}).encode("utf-8")
        return json.dumps({"weights": self.agent.export_weights(),
                           "arch": _ARCH_TAG}).encode("utf-8")

    def _handle_set_weights(self, msg: dict) -> bytes:
        if self.agent is None or not _HAS_TORCH:
            return json.dumps({"ok": False, "error": "no-agent"}).encode("utf-8")
        try:
            self.agent.load_weights(msg.get("weights", {}))
            return json.dumps({"ok": True}).encode("utf-8")
        except Exception as exc:
            return json.dumps({"ok": False, "error": str(exc)}).encode("utf-8")


if __name__ == "__main__":
    # Per-worker config via env vars (evaluated at module load, so they reach
    # the AgentDDPG default-arg paths). Run multiple instances by setting
    # NS3_ML_PORT and NS3_ML_AGENT_DIR to per-worker values.
    port = int(os.environ.get("NS3_ML_PORT", "5555"))
    service = MLService(bind_endpoint=f"tcp://*:{port}")
    fed_summary = (
        f"fedavg=on dir={_FEDAVG_DIR} worker_id={_WORKER_ID} "
        f"every={_FEDAVG_EVERY} steps"
        if _FEDAVG_DIR and _FEDAVG_EVERY > 0
        else "fedavg=off"
    )
    print(f"[ML] bound port={port} agent_dir={_AGENT_DIR} {fed_summary}")
    service.run()

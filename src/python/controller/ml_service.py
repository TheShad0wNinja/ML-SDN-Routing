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
- `{"cmd":"hello", "arch":"gnn-v1", "num_switches":N, "num_links":L,
    "node_feat_dim":5, "edge_feat_dim":3, "action_dim":L, "seed":S,
    "resume":bool, "checkpoint_every_n_ticks":K}` → `{"ok":true}`
- `{"cmd":"observe", "tick":t, "state":{...}, "prev_reward":r,
    "explore":bool}` → `{"action":[float * action_dim]}`
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


_AGENT_DIR = "scratch/data/agent"
_CKPT_PATH = os.path.join(_AGENT_DIR, "local.pt")
_REPLAY_PATH = os.path.join(_AGENT_DIR, "replay.pkl")
_METRICS_PATH = os.path.join(_AGENT_DIR, "metrics.csv")

# Bumped when the on-disk format changes in a way the loader can't fix up.
# The GNN refactor invalidates old MLP checkpoints and replay tuples.
_ARCH_TAG = "gnn-v1"

# Log-scale caps for unbounded queueing telemetry. log1p(1000) ≈ 6.91 — well
# past typical d_ms (~500ms practical max) and rtt_ms (~1s pathological).
_MAX_LOG_DELAY = math.log1p(1000.0)
_MAX_LOG_RTT = math.log1p(1000.0)

# GNN feature dims advertised by the C++ controller. Hardcoded here as defaults
# so the agent can still be instantiated for smoke tests; the real values come
# from the hello payload.
_NODE_FEAT_DIM = 5
_EDGE_FEAT_DIM = 3


# ---------------------------------------------------------------------------
# DDPG networks — GAT-based actor/critic. Weights depend only on feature
# dims (not N or L), which is the federated-averaging prerequisite.
# ---------------------------------------------------------------------------
if _HAS_TORCH:

    class _Actor(nn.Module):
        """Two-layer GATv2 encoder + per-canonical-edge head.

        Output is shape [E_total]: one action per canonical edge in the (possibly
        batched) graph. For a single-graph forward this matches the action_dim
        the C++ side expects.
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
            self.head = nn.Sequential(
                nn.Linear(2 * hidden + edge_dim, hidden),
                nn.ReLU(),
                nn.Linear(hidden, 1),
                nn.Tanh(),
            )

        def forward(self, data) -> torch.Tensor:
            h = F.relu(self.ln1(self.gat1(data.x, data.edge_index, data.edge_attr)))
            h = F.relu(self.ln2(self.gat2(h,      data.edge_index, data.edge_attr)))
            src, dst = data.canonical_edge_index  # each [E_total]
            feats = torch.cat([h[src], h[dst], data.canonical_edge_attr], dim=-1)
            return self.head(feats).squeeze(-1)

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
            # per-edge Q contribution: [h_src ‖ h_dst ‖ edge_attr ‖ action]
            self.edge_head = nn.Sequential(
                nn.Linear(2 * hidden + edge_dim + 1, hidden),
                nn.ReLU(),
                nn.Linear(hidden, hidden),
            )
            # global head: [pooled_edges ‖ u] → scalar Q
            self.global_head = nn.Sequential(
                nn.Linear(hidden + 1, hidden),
                nn.ReLU(),
                nn.Linear(hidden, 1),
            )

        def forward(self, data, action: torch.Tensor) -> torch.Tensor:
            h = F.relu(self.ln1(self.gat1(data.x, data.edge_index, data.edge_attr)))
            h = F.relu(self.ln2(self.gat2(h,      data.edge_index, data.edge_attr)))
            src, dst = data.canonical_edge_index
            feats = torch.cat([h[src], h[dst], data.canonical_edge_attr,
                               action.unsqueeze(-1)], dim=-1)
            edge_repr = self.edge_head(feats)  # [E_total, hidden]

            # Mean-pool edges per graph. For an unbatched single-graph call
            # data.batch is None, so we pool the whole thing.
            if getattr(data, "batch", None) is not None:
                # Map each canonical edge to its graph via its source node.
                edge_to_graph = data.batch[src]  # [E_total]
                num_graphs = int(data.batch.max().item()) + 1
                pooled = torch.zeros(num_graphs, edge_repr.size(1),
                                     device=edge_repr.device, dtype=edge_repr.dtype)
                pooled.index_add_(0, edge_to_graph, edge_repr)
                counts = torch.zeros(num_graphs, device=edge_repr.device,
                                     dtype=edge_repr.dtype)
                counts.index_add_(
                    0, edge_to_graph,
                    torch.ones_like(edge_to_graph, dtype=edge_repr.dtype))
                pooled = pooled / counts.clamp_min(1.0).unsqueeze(-1)
                u = data.u.view(num_graphs, 1)
            else:
                pooled = edge_repr.mean(dim=0, keepdim=True)  # [1, hidden]
                u = data.u.view(1, 1)

            return self.global_head(torch.cat([pooled, u], dim=-1)).squeeze(-1)


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
        self._noise_sigma = 0.3
        self._noise_sigma_min = 0.05
        self._noise_sigma_decay = 0.99995

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
        # Actions are per-canonical-edge; flatten across the batch in the same
        # order canonical_edge_index is concatenated.
        a_tensor = torch.from_numpy(np.concatenate(a, axis=0))
        r_tensor = torch.tensor(r, dtype=torch.float32).unsqueeze(-1)
        return s_batch, a_tensor, r_tensor, s2_batch

    def train_step(self):
        if len(self.replay) < max(self.warmup, self.batch_size):
            return None, None

        s, a, r, s2 = self._sample()
        with torch.no_grad():
            a2 = self.actor_target(s2)
            q_target = r + self.gamma * self.critic_target(s2, a2).unsqueeze(-1)
        q = self.critic(s, a).unsqueeze(-1)
        critic_loss = F.mse_loss(q, q_target)
        self.critic_opt.zero_grad()
        critic_loss.backward()
        torch.nn.utils.clip_grad_norm_(self.critic.parameters(), max_norm=1.0)
        self.critic_opt.step()

        actor_loss = -self.critic(s, self.actor(s)).mean()
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

        return float(critic_loss.item()), float(actor_loss.item())

    # ------------------------------------------------------------------
    # Acting
    # ------------------------------------------------------------------
    def act(self, data: "Data", explore: bool = True) -> "np.ndarray":
        """Return per-canonical-edge action vector for a single graph."""
        with torch.no_grad():
            # Wrap into a batch-of-one so downstream code can stay uniform.
            batch = Batch.from_data_list([data])
            a = self.actor(batch).cpu().numpy().astype(np.float32)

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
            if "noise_sigma" in blob:
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

    Expected schema (from C++ BuildMlStatePayload):
      state.node_index = [dpid_0, dpid_1, ...]   (frozen canonical order)
      state.per_switch = [{dpid, rho, d_ms, p_loss, depletion, echo_rtt_ns}, ...]
      state.per_link   = [{src, dst, tx_bps, capacity_bps, utilization, cost,
                           base_cost, delay_ms}, ...]
      state.residual_energy_stddev = float

    Returns Data with:
      x                    [N, 5]   node features
      edge_index           [2, 2L]  bidirectional message-passing edges
      edge_attr            [2L, 3]  features for message-passing edges
      canonical_edge_index [2, L]   one direction per link (matches action vec)
      canonical_edge_attr  [L, 3]   features for canonical edges
      u                    [1]      global residual_energy_stddev

    d_ms and echo_rtt are log1p-scaled to keep Tanh activations in their linear
    regime when queues back up — a queue spike from ~5ms to ~500ms would
    otherwise saturate the first GAT layer's attention scores.
    """
    node_index = state.get("node_index", []) or []
    dpid_to_idx = {int(d): i for i, d in enumerate(node_index)}
    n = max(len(node_index), 1)

    x = np.zeros((n, _NODE_FEAT_DIM), dtype=np.float32)
    for sw in state.get("per_switch", []) or []:
        idx = dpid_to_idx.get(int(sw.get("dpid", -1)), -1)
        if idx < 0:
            continue
        d_ms = max(0.0, float(sw.get("d_ms", 0.0)))
        d_ms_norm = math.log1p(d_ms) / _MAX_LOG_DELAY
        rtt_ms = max(0.0, float(sw.get("echo_rtt_ns", 0.0))) / 1.0e6
        rtt_norm = math.log1p(rtt_ms) / _MAX_LOG_RTT
        x[idx, 0] = float(sw.get("rho", 0.0))
        x[idx, 1] = float(d_ms_norm)
        x[idx, 2] = float(sw.get("p_loss", 0.0))
        x[idx, 3] = float(sw.get("depletion", 0.0))
        x[idx, 4] = float(rtt_norm)

    per_link = state.get("per_link", []) or []
    L = len(per_link)
    # Normalize cost by the max in this tick so the agent sees a unit-scale signal.
    max_cost = max((float(l.get("cost", 1.0)) for l in per_link), default=1.0)
    max_cost = max(max_cost, 1e-9)

    canon_src = np.zeros(max(L, 1), dtype=np.int64)
    canon_dst = np.zeros(max(L, 1), dtype=np.int64)
    edge_attr = np.zeros((max(L, 1), _EDGE_FEAT_DIM), dtype=np.float32)
    for i, l in enumerate(per_link):
        canon_src[i] = dpid_to_idx.get(int(l.get("src", -1)), 0)
        canon_dst[i] = dpid_to_idx.get(int(l.get("dst", -1)), 0)
        cap = float(l.get("capacity_bps", 0.0))
        tx = float(l.get("tx_bps", 0.0))
        edge_attr[i, 0] = float(l.get("utilization", 0.0))
        edge_attr[i, 1] = float(l.get("cost", 0.0)) / max_cost
        edge_attr[i, 2] = (tx / cap) if cap > 0.0 else 0.0

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
    bidir_edge_attr = np.concatenate([edge_attr[:L_eff], edge_attr[:L_eff]], axis=0)

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
    data.canonical_edge_attr = torch.from_numpy(edge_attr[:L_eff].copy())
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
                self.rep.close()
                self.ctx.term()
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
                tick, graph, prev_reward, explore = self._req_q.get(timeout=0.5)
            except queue.Empty:
                continue
            if self.agent is None or not _HAS_TORCH:
                continue

            # 1. Store (s, a, r, s') if we have a prior action cached.
            if self._prev_state is not None and self._prev_action is not None:
                self.agent.push(self._prev_state, self._prev_action,
                                prev_reward, graph)

            # 2. Train step (no-op until replay has warmup samples).
            #    Skip gradient updates entirely in eval mode — keeps the policy
            #    frozen so ML-vs-baseline comparisons are deterministic.
            t0 = time.perf_counter()
            if explore:
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

    # ------------------------------------------------------------------
    def _log_metrics(self, **row) -> None:
        os.makedirs(_AGENT_DIR, exist_ok=True)
        if self._metrics_fh is None:
            new_file = not os.path.exists(_METRICS_PATH)
            self._metrics_fh = open(_METRICS_PATH, "a", newline="")
            self._metrics_writer = csv.DictWriter(
                self._metrics_fh,
                fieldnames=["tick", "reward", "critic_loss", "actor_loss",
                            "mean_abs_action", "replay_size", "wall_clock_ms"],
            )
            if new_file:
                self._metrics_writer.writeheader()
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

        if not _HAS_TORCH:
            print(f"[ML] hello received but torch/torch_geometric missing — "
                  f"degraded mode ({_TORCH_IMPORT_ERROR})")
            return json.dumps({"ok": True, "degraded": True}).encode("utf-8")

        if arch and arch != _ARCH_TAG:
            print(f"[ML] WARNING: controller arch={arch!r} doesn't match "
                  f"service arch={_ARCH_TAG!r} — proceeding anyway.")

        try:
            self.agent = LocalDDPGAgent(
                action_dim=self.action_dim,
                node_dim=self.node_feat_dim,
                edge_dim=self.edge_feat_dim,
                seed=self.seed,
            )
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
            self._req_q.put_nowait((tick, graph, prev_reward, explore))
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
    service = MLService()
    service.run()

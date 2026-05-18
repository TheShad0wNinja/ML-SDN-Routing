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
- `{"cmd":"hello", "state_dim":N, "action_dim":M, "seed":S,
    "resume":bool, "checkpoint_every_n_ticks":K}` → `{"ok":true}`
- `{"cmd":"observe", "tick":t, "state":{...}, "prev_reward":r}` →
    `{"action":[float * action_dim]}`
- `{"cmd":"get_weights"}` → `{"weights":{name: list, ...}}`   (federation hook)
- `{"cmd":"set_weights", "weights":{...}}` → `{"ok":true}`     (federation hook)

If torch isn't installed, the service runs in degraded mode and returns tiny
random actions — useful for protocol-only smoke tests.
"""

from __future__ import annotations

import csv
import json
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

    _HAS_TORCH = True
    torch.set_num_threads(1)
except Exception as exc:  # pragma: no cover — only fires without torch
    _HAS_TORCH = False
    _TORCH_IMPORT_ERROR = exc


_AGENT_DIR = "scratch/data/agent"
_CKPT_PATH = os.path.join(_AGENT_DIR, "abilene_local.pt")
_REPLAY_PATH = os.path.join(_AGENT_DIR, "replay.pkl")
_METRICS_PATH = os.path.join(_AGENT_DIR, "metrics.csv")


# ---------------------------------------------------------------------------
# DDPG networks
# ---------------------------------------------------------------------------
if _HAS_TORCH:

    class _Actor(nn.Module):
        def __init__(self, state_dim: int, action_dim: int, hidden: int = 128):
            super().__init__()
            self.net = nn.Sequential(
                nn.Linear(state_dim, hidden),
                nn.ReLU(),
                nn.Linear(hidden, hidden),
                nn.ReLU(),
                nn.Linear(hidden, action_dim),
                nn.Tanh(),
            )

        def forward(self, s: torch.Tensor) -> torch.Tensor:
            return self.net(s)

    class _Critic(nn.Module):
        def __init__(self, state_dim: int, action_dim: int, hidden: int = 128):
            super().__init__()
            self.net = nn.Sequential(
                nn.Linear(state_dim + action_dim, hidden),
                nn.ReLU(),
                nn.Linear(hidden, hidden),
                nn.ReLU(),
                nn.Linear(hidden, 1),
            )

        def forward(self, s: torch.Tensor, a: torch.Tensor) -> torch.Tensor:
            return self.net(torch.cat([s, a], dim=-1))


class LocalDDPGAgent:
    """Single-agent DDPG with OU exploration noise."""

    def __init__(
        self,
        state_dim: int,
        action_dim: int,
        seed: int = 0,
        actor_lr: float = 1e-4,
        critic_lr: float = 1e-3,
        gamma: float = 0.99,
        tau: float = 0.005,
        replay_capacity: int = 50_000,
        batch_size: int = 64,
        # 100 transitions = ~100 s of sim time at the default mlIntervalS=1.0.
        # Anything higher and short runs (5-min Abilene scenarios) finish before
        # the agent ever takes a gradient step.
        warmup: int = 100,
    ):
        if not _HAS_TORCH:
            raise RuntimeError(
                f"LocalDDPGAgent requires torch / numpy; import failed: {_TORCH_IMPORT_ERROR}"
            )

        self.state_dim = state_dim
        self.action_dim = action_dim
        self.gamma = gamma
        self.tau = tau
        self.batch_size = batch_size
        self.warmup = warmup

        torch.manual_seed(seed)
        np.random.seed(seed)
        random.seed(seed)

        self.actor = _Actor(state_dim, action_dim)
        self.critic = _Critic(state_dim, action_dim)
        self.actor_target = _Actor(state_dim, action_dim)
        self.critic_target = _Critic(state_dim, action_dim)
        self.actor_target.load_state_dict(self.actor.state_dict())
        self.critic_target.load_state_dict(self.critic.state_dict())
        self.actor_opt = torch.optim.Adam(self.actor.parameters(), lr=actor_lr)
        self.critic_opt = torch.optim.Adam(self.critic.parameters(), lr=critic_lr)

        self.replay: deque = deque(maxlen=replay_capacity)

        # Ornstein–Uhlenbeck exploration noise.
        self._ou_state = np.zeros(action_dim, dtype=np.float32)
        self._ou_theta = 0.15
        self._ou_sigma = 0.2

    # ------------------------------------------------------------------
    # Replay + training
    # ------------------------------------------------------------------
    def push(self, s, a, r, s_next):
        self.replay.append((s.astype(np.float32), a.astype(np.float32),
                            float(r), s_next.astype(np.float32)))

    def _sample(self):
        idx = np.random.randint(0, len(self.replay), self.batch_size)
        batch = [self.replay[i] for i in idx]
        s, a, r, s2 = zip(*batch)
        return (torch.from_numpy(np.stack(s)),
                torch.from_numpy(np.stack(a)),
                torch.tensor(r, dtype=torch.float32).unsqueeze(-1),
                torch.from_numpy(np.stack(s2)))

    def train_step(self):
        if len(self.replay) < max(self.warmup, self.batch_size):
            return None, None

        s, a, r, s2 = self._sample()
        with torch.no_grad():
            a2 = self.actor_target(s2)
            q_target = r + self.gamma * self.critic_target(s2, a2)
        q = self.critic(s, a)
        critic_loss = F.mse_loss(q, q_target)
        self.critic_opt.zero_grad()
        critic_loss.backward()
        self.critic_opt.step()

        actor_loss = -self.critic(s, self.actor(s)).mean()
        self.actor_opt.zero_grad()
        actor_loss.backward()
        self.actor_opt.step()

        # Polyak updates
        with torch.no_grad():
            for p, pt in zip(self.actor.parameters(), self.actor_target.parameters()):
                pt.data.mul_(1 - self.tau).add_(self.tau * p.data)
            for p, pt in zip(self.critic.parameters(), self.critic_target.parameters()):
                pt.data.mul_(1 - self.tau).add_(self.tau * p.data)

        return float(critic_loss.item()), float(actor_loss.item())

    # ------------------------------------------------------------------
    # Acting
    # ------------------------------------------------------------------
    def act(self, state: "np.ndarray", explore: bool = True) -> "np.ndarray":
        with torch.no_grad():
            s = torch.from_numpy(state.astype(np.float32)).unsqueeze(0)
            a = self.actor(s).squeeze(0).numpy().astype(np.float32)

        if explore:
            dx = self._ou_theta * (-self._ou_state) + self._ou_sigma * np.random.randn(self.action_dim).astype(np.float32)
            self._ou_state = self._ou_state + dx
            a = a + self._ou_state
            a = np.clip(a, -1.0, 1.0)
        return a

    # ------------------------------------------------------------------
    # Persistence — federation hooks live here too.
    # ------------------------------------------------------------------
    def export_weights(self) -> dict:
        return {
            "actor": {k: v.cpu().numpy().tolist() for k, v in self.actor.state_dict().items()},
            "critic": {k: v.cpu().numpy().tolist() for k, v in self.critic.state_dict().items()},
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

    def save_checkpoint(self, path: str = _CKPT_PATH, replay_path: str = _REPLAY_PATH) -> None:
        os.makedirs(os.path.dirname(path), exist_ok=True)
        tmp = path + ".tmp"
        torch.save({
            "actor": self.actor.state_dict(),
            "critic": self.critic.state_dict(),
            "actor_target": self.actor_target.state_dict(),
            "critic_target": self.critic_target.state_dict(),
            "actor_opt": self.actor_opt.state_dict(),
            "critic_opt": self.critic_opt.state_dict(),
        }, tmp)
        os.replace(tmp, path)

        tmp_r = replay_path + ".tmp"
        with open(tmp_r, "wb") as f:
            pickle.dump(list(self.replay), f, protocol=pickle.HIGHEST_PROTOCOL)
        os.replace(tmp_r, replay_path)

    def maybe_load_checkpoint(self, path: str = _CKPT_PATH, replay_path: str = _REPLAY_PATH) -> bool:
        if not os.path.exists(path):
            return False
        try:
            blob = torch.load(path, map_location="cpu")
            self.actor.load_state_dict(blob["actor"])
            self.critic.load_state_dict(blob["critic"])
            self.actor_target.load_state_dict(blob["actor_target"])
            self.critic_target.load_state_dict(blob["critic_target"])
            self.actor_opt.load_state_dict(blob["actor_opt"])
            self.critic_opt.load_state_dict(blob["critic_opt"])
        except Exception as exc:
            print(f"[ML] checkpoint load failed: {exc}")
            return False
        if os.path.exists(replay_path):
            try:
                with open(replay_path, "rb") as f:
                    items = pickle.load(f)
                self.replay = deque(items, maxlen=self.replay.maxlen)
            except Exception as exc:
                print(f"[ML] replay load failed: {exc}")
        return True


# ---------------------------------------------------------------------------
# Service
# ---------------------------------------------------------------------------
def _flatten_state(state: dict) -> "np.ndarray":
    """Flatten the controller's JSON state into a 1D numpy vector.

    Layout (must match BuildMlStatePayload + MlSendHello state_dim):
      per_switch[i].{rho, d_ms, p_loss, residual_energy_frac, depletion,
                     echo_rtt_s, mu_max_gbps}                   (7 each)
      per_link[i].{utilization, cost_norm, tx_share}            (3 each)
      residual_energy_stddev                                    (1)
    """
    pieces = []
    for sw in state.get("per_switch", []):
        pieces.extend([
            sw.get("rho", 0.0),
            sw.get("d_ms", 0.0),
            sw.get("p_loss", 0.0),
            sw.get("residual_energy_frac", 1.0),
            sw.get("depletion", 0.0),
            sw.get("echo_rtt_ns", 0.0) / 1e9,        # scale to seconds
            sw.get("mu_max_bps", 0.0) / 1e9,         # scale to Gbps
        ])

    links = state.get("per_link", [])
    # Normalize cost by the max in this tick so the agent sees a unit-scale signal.
    max_cost = max((float(l.get("cost", 1.0)) for l in links), default=1.0)
    max_cost = max(max_cost, 1e-9)
    for l in links:
        cap = float(l.get("capacity_bps", 0.0))
        tx = float(l.get("tx_bps", 0.0))
        pieces.extend([
            float(l.get("utilization", 0.0)),
            float(l.get("cost", 0.0)) / max_cost,
            (tx / cap) if cap > 0.0 else 0.0,
        ])

    pieces.append(float(state.get("residual_energy_stddev", 0.0)))

    return np.array(pieces, dtype=np.float32) if _HAS_TORCH else pieces  # type: ignore[return-value]


class MLService:
    """Threaded REP-socket DDPG service."""

    def __init__(self, bind_endpoint: str = "tcp://*:5555"):
        self.ctx = zmq.Context()
        self.rep = self.ctx.socket(zmq.REP)
        self.rep.bind(bind_endpoint)

        # Agent state — populated by `hello`.
        self.agent: LocalDDPGAgent | None = None
        self.state_dim = 0
        self.action_dim = 0
        self.checkpoint_every = 60
        self.seed = 0

        # Worker thread coordination.
        # Tuple is (tick, state_vec, prev_reward, explore).
        self._req_q: "queue.Queue[tuple[int, np.ndarray, float, bool]]" = queue.Queue()
        self._stop = threading.Event()
        self._action_lock = threading.Lock()
        # Cached "best action so far". Producer = worker, consumer = main.
        self._last_action: "np.ndarray | None" = None
        self._prev_state: "np.ndarray | None" = None
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
                tick, state_vec, prev_reward, explore = self._req_q.get(timeout=0.5)
            except queue.Empty:
                continue
            if self.agent is None or not _HAS_TORCH:
                continue

            # 1. Store (s, a, r, s') if we have a prior action cached.
            if self._prev_state is not None and self._prev_action is not None:
                self.agent.push(self._prev_state, self._prev_action,
                                prev_reward, state_vec)

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
            next_action = self.agent.act(state_vec, explore=explore)
            with self._action_lock:
                self._last_action = next_action

            self._prev_state = state_vec
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
        self.state_dim = int(msg.get("state_dim", 0))
        self.action_dim = int(msg.get("action_dim", 0))
        self.checkpoint_every = int(msg.get("checkpoint_every_n_ticks", 60))
        self.seed = int(msg.get("seed", 0))
        resume = bool(msg.get("resume", True))

        if not _HAS_TORCH:
            print(f"[ML] hello received but torch missing — degraded mode "
                  f"({_TORCH_IMPORT_ERROR})")
            return json.dumps({"ok": True, "degraded": True}).encode("utf-8")

        try:
            self.agent = LocalDDPGAgent(self.state_dim, self.action_dim, seed=self.seed)
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

        print(f"[ML] hello: state_dim={self.state_dim} action_dim={self.action_dim} "
              f"seed={self.seed} resumed={resumed}")
        return json.dumps({"ok": True, "resumed": resumed}).encode("utf-8")

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

        state_vec = _flatten_state(state)
        # If the dim drifted (shouldn't, after hello), pad/truncate.
        if state_vec.shape[0] != self.state_dim and self.state_dim > 0:
            padded = np.zeros(self.state_dim, dtype=np.float32)
            n = min(state_vec.shape[0], self.state_dim)
            padded[:n] = state_vec[:n]
            state_vec = padded

        # Enqueue to worker — never block for more than a microsecond.
        try:
            self._req_q.put_nowait((tick, state_vec, prev_reward, explore))
        except queue.Full:
            pass

        with self._action_lock:
            action = (self._last_action
                      if self._last_action is not None
                      else np.zeros(self.action_dim, dtype=np.float32))
            action_list = action.tolist()
        return json.dumps({"action": action_list}).encode("utf-8")

    def _handle_get_weights(self) -> bytes:
        # Federation hook stub: returns weights if the agent exists, else empty.
        if self.agent is None or not _HAS_TORCH:
            return json.dumps({"weights": {}}).encode("utf-8")
        return json.dumps({"weights": self.agent.export_weights()}).encode("utf-8")

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

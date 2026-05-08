"""
Main entry point for the Python controller service.

Architecture after refactoring:
- C++ (ns-3 ZmqOpenFlowController) handles all control logic:
  * Topology learning (LLDP discovery)
  * Host location learning
  * Path computation (shortest path via Dijkstra)
  * Flow installation
  * Packet forwarding (packet-out generation)

- Python (MLService) handles ML/inference only:
  * Receives inference requests from C++
  * Runs ML models (optional congestion prediction, anomaly detection, etc.)
  * Returns predictions (optional)
  * Falls back to C++ defaults if ML is unavailable or slow

This separation improves performance and determinism in the simulator.
"""

from .ml_service import MLService


if __name__ == "__main__":
    service = MLService()
    service.run()

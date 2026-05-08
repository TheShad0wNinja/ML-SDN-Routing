"""Controller package for ns-3 OFSwitch13 simulator.

After refactoring, this package contains:
- MLService: ML/inference service for optional model predictions
- ml_service module: ZMQ endpoint for C++ controller to call

C++ now handles all control logic:
- Topology learning via LLDP
- Host location tracking
- Path computation
- Flow installation
"""

from .ml_service import MLService

__all__ = ["MLService"]



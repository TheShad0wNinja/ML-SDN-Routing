"""
ML Service for ns-3 SDN Controller

Simplified ZMQ service that provides machine learning inference for the C++ controller.
The C++ controller now handles all control logic (topology, host learning, routing).
This service is called only for ML-based decision augmentation or monitoring.

Wire protocol:
  REQUEST:  Empty or custom feature vector (application-specific)
  REPLY:    Empty or ML prediction/action bytes
"""

from __future__ import annotations

import zmq
import struct
import json
from typing import Optional


class MLService:
    """
    Simplified ML inference server.
    
    In the current architecture, C++ has taken over all control-plane logic.
    This service is a placeholder for ML-based enhancements (e.g., congestion 
    prediction, anomaly detection, traffic engineering) that can run asynchronously.
    """

    def __init__(self, bind_endpoint: str = "tcp://*:5555"):
        self.ctx = zmq.Context()
        self.rep = self.ctx.socket(zmq.REP)
        self.rep.bind(bind_endpoint)
        self.request_count = 0

    def run(self) -> None:
        """Main loop: receive requests, return ML predictions."""
        print("ML Service ready. Waiting for inference requests from C++ controller...")
        try:
            while True:
                # Receive request from C++
                data = self.rep.recv()
                self.request_count += 1
                
                # Process request and generate response
                response = self._process_request(data)
                
                # Send response back to C++
                self.rep.send(response)
        except KeyboardInterrupt:
            print("ML Service shutting down.")
        finally:
            self.rep.close()
            self.ctx.term()

    def _process_request(self, data: bytes) -> bytes:
        """
        Process an ML inference request.
        
        Args:
            data: Request payload from C++ (can be empty or feature vector)
        
        Returns:
            Response payload (empty or ML predictions)
        """
        # If empty or heartbeat, return empty
        if not data or data == b"PING":
            return b""
        
        # Try to parse as JSON feature vector for custom ML logic
        try:
            features = json.loads(data.decode("utf-8"))
            # Example: compute congestion score or anomaly detection
            prediction = self._ml_inference(features)
            return json.dumps(prediction).encode("utf-8")
        except (json.JSONDecodeError, UnicodeDecodeError):
            # Unknown format; return empty (C++ will use defaults)
            return b""

    def _ml_inference(self, features: dict) -> dict:
        """
        Placeholder for ML model inference.
        
        In production, this would call a trained ML model to make predictions
        (e.g., congestion forecasting, traffic classification, anomaly detection).
        
        Args:
            features: Dictionary of features from C++ controller
        
        Returns:
            Dictionary of ML predictions/actions
        """
        # For now, return a simple pass-through
        # In production:
        #   - Load a trained model (TensorFlow, PyTorch, etc.)
        #   - Extract features from the input dict
        #   - Run inference and return predictions
        return {
            "model": "placeholder",
            "confidence": 1.0,
            "action": "use_c++_default"
        }

    def __del__(self):
        try:
            self.rep.close()
            self.ctx.term()
        except:
            pass


if __name__ == "__main__":
    service = MLService()
    service.run()

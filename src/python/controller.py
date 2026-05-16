"""
Entrypoint wrapper.

The implementation lives in `scratch/python/controller/` (package). This script
exists so existing workflows keep working:

  python scratch/python/controller.py
"""

from controller.ml_service import MLService


if __name__ == "__main__":
    MLService().run()

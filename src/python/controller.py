"""
Entrypoint wrapper.

The implementation lives in `scratch/python/controller/` (package) to keep the
controller logic modular and testable. This file remains runnable so existing
workflows keep working:

  python scratch/python/controller.py
"""

from controller import SDNController


if __name__ == "__main__":
    SDNController()

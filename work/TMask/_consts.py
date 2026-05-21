"""Shared product constants for the TMask Nuke plugin."""

from __future__ import annotations

import os
from pathlib import Path

PACKAGE_PATH = Path(__file__).resolve().parent
INSTALLATION_PATH = str(PACKAGE_PATH)

PRODUCT_NAME = "TMask"
PRODUCT_VERSION = "0.1"
PRODUCT_RELEASE_YEAR = "2026"
PRODUCT_VENDOR = "Thomas Petroni"
PRODUCT_VENDOR_URL = "https://www.linkedin.com/in/thomas-petroni/"

NODE_CLASS_NAME = PRODUCT_NAME
MENU_NAME = PRODUCT_NAME
PLUGIN_BIN_DIRECTORY = "bin_new4"

PLUGIN_LOADED_ENV_VAR = "TMASK_LOADED"
PLUGIN_BINARY_PATH_ENV_VAR = "TMASK_PLUGIN_BIN_PATH"
HOOKS_SETUP_ENV_VAR = "TMASK_HOOKS_SETUP"


def normalized_path(path: str) -> str:
    """Normalize a filesystem path for Nuke plugin registration."""
    return path.replace(os.sep, "/")

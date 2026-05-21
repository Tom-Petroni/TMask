"""Plugin creation script for TMask menu in Nuke."""

import logging

try:
    from TMask._menu_creator import add_menu
except Exception:
    from _menu_creator import add_menu

logger = logging.getLogger(__name__)

try:
    add_menu()
except Exception:  # pragma: no cover - Nuke runtime dependency
    logger.exception("Unexpected failure while creating the TMask menu.")

"""Functions that handle creation of the Nuke menu."""

import logging
import os

import nuke  # ty: ignore[unresolved-import]

try:
    from TMask._consts import MENU_NAME, NODE_CLASS_NAME, PLUGIN_LOADED_ENV_VAR
    from TMask._plugin_loader import ensure_node_class_loaded
except Exception:
    from _consts import MENU_NAME, NODE_CLASS_NAME, PLUGIN_LOADED_ENV_VAR
    from _plugin_loader import ensure_node_class_loaded

logger = logging.getLogger(__name__)


def create_node():
    """Create node and force-load plugin binary first."""
    try:
        ensure_node_class_loaded()
        nuke.createNode(NODE_CLASS_NAME)
    except Exception as error:
        nuke.tprint("[TMask] Unable to create node '{}': {}".format(NODE_CLASS_NAME, error))


def _create_menu():
    """Create the Nuke menu and add the command."""
    toolbar = nuke.menu("Nodes")
    menu = toolbar.findItem(MENU_NAME)
    if menu is None:
        menu = toolbar.addMenu(MENU_NAME)

    command_path = "{}/{}".format(MENU_NAME, NODE_CLASS_NAME)
    if toolbar.findItem(command_path) is None:
        callback = "import {} as _tm_menu; _tm_menu.create_node()".format(__name__)
        menu.addCommand(
            NODE_CLASS_NAME,
            callback,
        )


def add_menu():
    """Always create the menu; log plugin load state for diagnostics."""
    _create_menu()

    if os.getenv(PLUGIN_LOADED_ENV_VAR) != "1":
        logger.warning("TMask menu created, but plugin binary is not loaded yet.")

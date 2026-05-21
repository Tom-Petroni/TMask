"""Viewer pick helpers for the native TMask node."""

from __future__ import annotations

import logging
import os

import nuke  # ty: ignore[unresolved-import]

try:
    from _consts import HOOKS_SETUP_ENV_VAR, NODE_CLASS_NAME
except Exception:
    from TMask._consts import HOOKS_SETUP_ENV_VAR, NODE_CLASS_NAME

logger = logging.getLogger(__name__)

_PICK_KNOB_NAMES = {"pick", "p1_pick"}
_CENTER_KNOB_CANDIDATES = ("center", "p1_center")


def register_picker_callbacks():
    """Register TMask knob callback once."""
    if os.getenv(HOOKS_SETUP_ENV_VAR) == "1":
        return
    nuke.addKnobChanged(_on_knob_changed, nodeClass=NODE_CLASS_NAME)
    os.environ[HOOKS_SETUP_ENV_VAR] = "1"


def _on_knob_changed():
    node = _safe_this_node()
    knob = _safe_this_knob()
    if node is None or knob is None:
        return

    try:
        if node.Class() != NODE_CLASS_NAME:
            return
    except Exception:
        # Nuke may trigger callbacks while PythonObject is detached.
        return

    try:
        knob_name = knob.name()
    except Exception:
        return

    if knob_name not in _PICK_KNOB_NAMES:
        return

    _pick_center_xy(node)


def _pick_center_xy(node):
    sample_pos = _viewer_sample_position()
    if sample_pos is None:
        nuke.tprint("[TMask] Viewer sample position not found. Pick in the Viewer first.")
        return

    center_knob = _resolve_center_knob(node)
    if center_knob is None:
        nuke.tprint("[TMask] Center knob not found.")
        return

    x_pos, y_pos = sample_pos
    try:
        center_knob.setValue(float(x_pos), 0)
        center_knob.setValue(float(y_pos), 1)
    except Exception as error:
        nuke.tprint("[TMask] Failed to set center: {}".format(error))
        return

    nuke.tprint("[TMask] Center picked at ({:.2f}, {:.2f}).".format(float(x_pos), float(y_pos)))


def _resolve_center_knob(node):
    for knob_name in _CENTER_KNOB_CANDIDATES:
        knob = node.knob(knob_name)
        if knob is not None:
            return knob
    return None


def _viewer_sample_position():
    viewer = nuke.activeViewer()
    if viewer is None:
        return None
    viewer_node = viewer.node() if hasattr(viewer, "node") else None
    if viewer_node is None:
        return None

    for knob_name in ("colour_sample_bbox", "color_sample_bbox"):
        bbox_knob = viewer_node.knob(knob_name)
        if bbox_knob is None:
            continue
        try:
            bbox = bbox_knob.value()
        except Exception:
            continue
        if isinstance(bbox, (tuple, list)) and len(bbox) >= 4:
            x_pos = (float(bbox[0]) + float(bbox[2])) * 0.5
            y_pos = (float(bbox[1]) + float(bbox[3])) * 0.5
            return x_pos, y_pos
    return None


def _safe_this_node():
    try:
        return nuke.thisNode()
    except Exception:
        return None


def _safe_this_knob():
    try:
        return nuke.thisKnob()
    except Exception:
        return None

#!/usr/bin/env python3
"""Inspect, edit or activate an OpenCPN control through Linux AT-SPI.

This is an optional manual/conformance helper. It does not form part of the
runtime and is useful on Wayland where synthetic pointer tools are unavailable.
"""

import argparse
import sys

import gi

gi.require_version("Atspi", "2.0")
from gi.repository import Atspi  # noqa: E402
from gi.repository import GLib  # noqa: E402


def children(accessible):
    for index in range(accessible.get_child_count()):
        child = accessible.get_child_at_index(index)
        if child is not None:
            yield child


def walk(accessible, path="", depth=0):
    if depth > 12:
        return
    yield accessible, path
    for index, child in enumerate(children(accessible)):
        yield from walk(child, f"{path}/{index}", depth + 1)


def actions(accessible):
    try:
        action = accessible.get_action_iface()
        if action is None:
            return []
        return [
            action.get_localized_name(index)
            for index in range(action.get_n_actions())
        ]
    except GLib.Error:
        return []


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--activate", help="exact accessible control name")
    text_source = parser.add_mutually_exclusive_group()
    text_source.add_argument(
        "--set-text", help="replace text in an editable control"
    )
    text_source.add_argument(
        "--set-text-stdin",
        action="store_true",
        help="read replacement text from stdin (keeps secrets out of argv)",
    )
    parser.add_argument(
        "--set-text-role",
        help="accessible role of the editable control (for example, text)",
    )
    parser.add_argument(
        "--match-index",
        type=int,
        default=0,
        help="zero-based match to use when an unnamed role occurs more than once",
    )
    parser.add_argument("--focus-title", help="exact accessible frame title")
    parser.add_argument(
        "--frame-title",
        help="limit inspection/activation to this exact top-level frame",
    )
    parser.add_argument("--application", default="opencpn")
    args = parser.parse_args()
    replacement_text = sys.stdin.read() if args.set_text_stdin else args.set_text

    Atspi.init()
    desktop = Atspi.get_desktop(0)
    applications = [
        item
        for item in children(desktop)
        if (item.get_name() or "").casefold() == args.application.casefold()
    ]
    if not applications:
        print(f"application not found: {args.application}", file=sys.stderr)
        return 2

    matches = []
    for app_index, application in enumerate(applications):
        roots = [(application, str(app_index))]
        if args.frame_title:
            roots = [
                (item, f"{app_index}/{index}")
                for index, item in enumerate(children(application))
                if (item.get_name() or "") == args.frame_title
            ]
        for root, root_path in roots:
            for item, path in walk(root, root_path):
                name = item.get_name() or ""
                action_names = actions(item)
                if args.focus_title:
                    if name == args.focus_title:
                        matches.append((item, path, []))
                elif replacement_text is not None:
                    if item.get_role_name() == args.set_text_role:
                        try:
                            editable = item.get_editable_text_iface()
                        except GLib.Error:
                            editable = None
                        if editable is not None:
                            matches.append((item, path, []))
                elif args.activate:
                    if name == args.activate and action_names:
                        matches.append((item, path, action_names))
                elif name or action_names:
                    print(
                        f"{path} name={name!r} role={item.get_role_name()!r} "
                        f"actions={action_names!r}"
                    )

    if not args.activate and not args.focus_title and replacement_text is None:
        return 0
    target = args.activate or args.focus_title or args.set_text_role
    required_matches = 1 if replacement_text is None else args.match_index + 1
    if len(matches) < required_matches or (
        replacement_text is None and len(matches) != 1
    ):
        print(
            f"expected one control named {target!r}, "
            f"found {len(matches)}",
            file=sys.stderr,
        )
        return 3
    item, path, action_names = matches[
        args.match_index if replacement_text is not None else 0
    ]
    if replacement_text is not None:
        editable = item.get_editable_text_iface()
        if editable is None or not editable.set_text_contents(replacement_text):
            print(f"AT-SPI text edit failed at {path}", file=sys.stderr)
            return 4
        print(f"set text at {path}")
        return 0
    if args.focus_title:
        component = item.get_component_iface()
        if component is None or not component.grab_focus():
            print(f"AT-SPI focus failed at {path}", file=sys.stderr)
            return 4
        print(f"focused {args.focus_title!r} at {path}")
        return 0
    if not item.do_action(0):
        print(f"AT-SPI action failed at {path}: {action_names}", file=sys.stderr)
        return 4
    print(f"activated {args.activate!r} at {path} using {action_names[0]!r}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())

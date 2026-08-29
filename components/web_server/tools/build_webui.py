#!/usr/bin/env python3
"""
Build-time assembler for ESP32 BLE Gateway web UI.

Reads modular source files from www_src/ and generates:
  - dashboard.html (assembled from shell + views + partials + JS)
"""

import argparse
import os
import re
import sys


def read_file(path):
    """Read file content, raise on failure."""
    with open(path, 'r', encoding='utf-8') as f:
        return f.read()


def write_file(path, content):
    """Write content to file."""
    os.makedirs(os.path.dirname(path), exist_ok=True)
    with open(path, 'w', encoding='utf-8') as f:
        f.write(content)


def resolve_includes(html, base_dir, seen=None):
    """Recursively resolve <!-- @include path --> directives."""
    if seen is None:
        seen = set()

    def replacer(match):
        rel_path = match.group(1).strip()
        abs_path = os.path.normpath(os.path.join(base_dir, rel_path))

        if abs_path in seen:
            raise ValueError(f"Circular include detected: {rel_path}")
        seen.add(abs_path)

        if not os.path.isfile(abs_path):
            raise FileNotFoundError(f"Include target not found: {rel_path} (resolved to {abs_path})")

        included = read_file(abs_path)
        included_dir = os.path.dirname(abs_path)
        return resolve_includes(included, included_dir, seen)

    return re.sub(r'<!--\s*@include\s+(\S+)\s*-->', replacer, html)


def resolve_js_includes(html, base_dir):
    """Resolve <!-- @js path --> directives inside the shell's script block.
    
    base_dir is the directory of the HTML file containing the directives.
    JS paths like 'core/state.js' are resolved relative to base_dir's parent
    (the www_src/dashboard/ directory) since shell.html is in www_src/dashboard/
    and JS files are in www_src/dashboard/js/.
    """
    # The shell.html is at www_src/dashboard/shell.html
    # JS directives use paths like 'core/state.js' which should resolve to
    # www_src/dashboard/js/core/state.js
    js_base = os.path.join(base_dir, 'js')
    
    def replacer(match):
        rel_path = match.group(1).strip()
        abs_path = os.path.normpath(os.path.join(js_base, rel_path))

        if not os.path.isfile(abs_path):
            raise FileNotFoundError(f"JS module not found: {rel_path} (resolved to {abs_path})")

        js_content = read_file(abs_path)
        if re.search(r'</script\s*>', js_content, flags=re.IGNORECASE):
            raise ValueError(f"JS module contains a closing script tag: {rel_path}")

        # shell.html owns the enclosing <script> element. Adding another pair of
        # script tags here creates invalid nested scripts and makes the browser
        # render everything after the first module as page text.
        return f"\n// Source: {rel_path}\n{js_content}\n"

    return re.sub(r'<!--\s*@js\s+(\S+)\s*-->', replacer, html)


def build_dashboard(source_dir, output_path):
    """Build dashboard.html from modular sources."""
    shell_path = os.path.join(source_dir, 'dashboard', 'shell.html')
    if not os.path.isfile(shell_path):
        raise FileNotFoundError(f"Dashboard shell not found: {shell_path}")

    html = read_file(shell_path)

    # First resolve @include directives (HTML partials/views)
    html = resolve_includes(html, os.path.dirname(shell_path))

    # Then resolve @js directives (JavaScript modules)
    html = resolve_js_includes(html, os.path.dirname(shell_path))

    # Validate: no unresolved markers
    unresolved = re.findall(r'<!--\s*@(include|js)\s+(\S+)\s*-->', html)
    if unresolved:
        raise ValueError(f"Unresolved markers found: {unresolved}")

    if len(re.findall(r'<script(?:\s|>)', html, flags=re.IGNORECASE)) != 1:
        raise ValueError("Generated dashboard must contain exactly one script block")
    if len(re.findall(r'</script\s*>', html, flags=re.IGNORECASE)) != 1:
        raise ValueError("Generated dashboard must contain exactly one closing script tag")

    # Validate: contains expected root elements
    if 'id="view-devices"' not in html:
        raise ValueError("Generated dashboard missing devices view")
    if 'id="view-settings"' not in html:
        raise ValueError("Generated dashboard missing settings view")
    if 'id="view-scanner"' not in html:
        raise ValueError("Generated dashboard missing scanner view")
    if 'id="view-device-detail"' not in html:
        raise ValueError("Generated dashboard missing device detail view")

    # Add banner
    banner = (
        "<!-- GENERATED FILE - DO NOT EDIT -->\n"
        "<!-- source: components/web_server/www_src/ -->\n\n"
    )
    html = banner + html

    write_file(output_path, html)
    print(f"Generated dashboard: {output_path} ({len(html)} bytes)")


def main():
    parser = argparse.ArgumentParser(description='Build web UI assets for ESP32 gateway')
    parser.add_argument('--source', required=True, help='Source directory (www_src/)')
    parser.add_argument('--dashboard-out', required=True, help='Output path for dashboard.html')
    args = parser.parse_args()

    source_dir = os.path.abspath(args.source)

    if not os.path.isdir(source_dir):
        print(f"Error: Source directory not found: {source_dir}", file=sys.stderr)
        sys.exit(1)

    try:
        build_dashboard(source_dir, args.dashboard_out)
    except Exception as e:
        print(f"Error: Dashboard build failed: {e}", file=sys.stderr)
        sys.exit(1)

    print("Web UI build completed successfully")


if __name__ == '__main__':
    main()

#!/usr/bin/env python3
"""
Upload all tracked files in the current git repo to a GitHub repository
via the Git Database REST API (api.github.com).

This is needed because github.com (port 443) is blocked by firewall,
but api.github.com is accessible.

Usage:
    python3 tools/github_upload.py <token> <owner> <repo>
"""

import os
import sys
import json
import base64
import socket
import subprocess
import time
import urllib.request
import urllib.error

API_BASE = "https://api.github.com"

def api_call(token, method, path, data=None, max_retries=5):
    """Make an authenticated GitHub API request with retry logic."""
    url = f"{API_BASE}{path}"
    headers = {
        "Authorization": f"token {token}",
        "Accept": "application/vnd.github+json",
        "Content-Type": "application/json",
    }
    body = json.dumps(data).encode() if data else None

    for attempt in range(max_retries):
        req = urllib.request.Request(url, data=body, headers=headers, method=method)
        try:
            with urllib.request.urlopen(req, timeout=120) as resp:
                return json.loads(resp.read())
        except urllib.error.HTTPError as e:
            error_body = e.read().decode()
            if e.code == 422 and "already exists" in error_body:
                # Blob already created, try to extract sha
                print(f"  (already exists, retrying)")
            else:
                print(f"API error {e.code}: {error_body[:500]}", file=sys.stderr)
                raise
        except (urllib.error.URLError, socket.timeout, TimeoutError) as e:
            if attempt < max_retries - 1:
                wait = 3 * (attempt + 1)
                print(f"  Retry {attempt+1}/{max_retries} after {wait}s (timeout/error)")
                time.sleep(wait)
            else:
                print(f"  Failed after {max_retries} retries: {e}", file=sys.stderr)
                raise
    return None

def get_tracked_files():
    """Get list of all files tracked by git (including deleted ones excluded)."""
    result = subprocess.run(
        ["git", "ls-files"],
        capture_output=True, text=True
    )
    files = [f for f in result.stdout.strip().split("\n") if f]
    return files

def create_blobs(token, owner, repo, file_list, project_dir):
    """Create a blob for each file and return tree entries."""
    tree_entries = []

    for i, filepath in enumerate(file_list):
        full_path = os.path.join(project_dir, filepath)

        if not os.path.exists(full_path):
            print(f"  SKIP (missing): {filepath}")
            continue

        # Check if file is binary
        is_binary = False
        try:
            with open(full_path, "rb") as f:
                chunk = f.read(8192)
                if b'\x00' in chunk:
                    is_binary = True
        except:
            print(f"  SKIP (cannot read): {filepath}")
            continue

        with open(full_path, "rb") as f:
            content = f.read()

        if is_binary:
            # Use base64 encoding for binary files
            content_b64 = base64.b64encode(content).decode()
            blob_data = {"content": content_b64, "encoding": "base64"}
        else:
            # Use utf-8 for text files
            content_text = content.decode("utf-8", errors="replace")
            blob_data = {"content": content_text, "encoding": "utf-8"}

        print(f"  [{i+1}/{len(file_list)}] Creating blob: {filepath} ({len(content)} bytes, {'binary' if is_binary else 'text'})")

        blob = api_call(token, "POST", f"/repos/{owner}/{repo}/git/blobs", blob_data)
        blob_sha = blob["sha"]

        tree_entries.append({
            "path": filepath,
            "mode": "100644",
            "type": "blob",
            "sha": blob_sha,
        })

    return tree_entries

def main():
    if len(sys.argv) != 4:
        print(f"Usage: {sys.argv[0]} <token> <owner> <repo>")
        sys.exit(1)

    token = sys.argv[1]
    owner = sys.argv[2]
    repo = sys.argv[3]

    project_dir = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
    os.chdir(project_dir)

    print(f"Project directory: {project_dir}")
    print(f"Target: {owner}/{repo}")
    print()

    # Step 1: Get list of tracked files
    files = get_tracked_files()
    print(f"Step 1: Found {len(files)} tracked files")
    for f in files:
        print(f"  - {f}")
    print()

    # Step 1.5: Initialize empty repo by creating README via Contents API
    print("Step 1.5: Initializing repository (creating first file via Contents API)...")
    readme_path = os.path.join(project_dir, "README.md")
    with open(readme_path, "rb") as f:
        readme_content = f.read().decode("utf-8", errors="replace")
    init_data = {
        "message": "Initial commit",
        "content": base64.b64encode(readme_content.encode()).decode(),
    }
    try:
        api_call(token, "PUT", f"/repos/{owner}/{repo}/contents/README.md", init_data)
        print("  README.md created via Contents API (repo initialized)")
    except urllib.error.HTTPError as e:
        if e.code == 422:
            print("  README.md already exists, continuing...")
        else:
            raise
    print()

    # Step 2: Create blobs for each file
    print("Step 2: Creating blobs...")
    tree_entries = create_blobs(token, owner, repo, files, project_dir)
    print(f"  Created {len(tree_entries)} blobs")
    print()

    # Step 3: Create tree
    print("Step 3: Creating tree...")
    tree_data = {"tree": tree_entries}
    tree = api_call(token, "POST", f"/repos/{owner}/{repo}/git/trees", tree_data)
    tree_sha = tree["sha"]
    print(f"  Tree SHA: {tree_sha}")
    print()

    # Step 4: Create commit (no parent — initial commit)
    print("Step 4: Creating commit...")
    commit_data = {
        "message": "Fix FFT engine bugs and add HRTF spatial audio processing\n\n"
                   "Bug fixes in dsp/fft_conv.c:\n"
                   "- Fix butterfly index: use x[i+k] instead of x[i] for k-th butterfly\n"
                   "  in each group (Radix-2 DIT FFT)\n"
                   "- Fix inverse FFT twiddle conjugate: only flip imaginary part\n"
                   "- Fix CONV_HISTORY_LEN: 511 -> 512 (FFT_N - CONV_INPUT_LEN)\n"
                   "- Remove unnecessary zero-padding of fft_buf[FFT_N-1]\n\n"
                   "New files:\n"
                   "- dsp/fft_conv.c/h: FFT engine + overlap-save partitioned convolution\n"
                   "- host/sofa_loader.c/h: SOFA file loader for HRTF data\n"
                   "- azimuth_map.h: azimuth angle mapping table\n"
                   "- tools/fft_test.c: PC-side FFT regression test\n"
                   "- tools/sofa_to_bin.py: SOFA to HRIR binary converter\n"
                   "- README.md, .gitignore\n\n"
                   "Modified:\n"
                   "- host/App.c, dsp/Server.c, shared/AppCommon.h, makefiles\n\n"
                   "Removed:\n"
                   "- client.c (obsolete, replaced by host/App.c)",
        "tree": tree_sha,
    }

    # Try to get existing commits (if repo has any)
    try:
        commits = api_call(token, "GET", f"/repos/{owner}/{repo}/commits?per_page=1")
        if commits and len(commits) > 0:
            parent_sha = commits[0]["sha"]
            commit_data["parents"] = [parent_sha]
            print(f"  Parent commit: {parent_sha}")
    except:
        print("  No parent commits (initial commit)")

    commit = api_call(token, "POST", f"/repos/{owner}/{repo}/git/commits", commit_data)
    commit_sha = commit["sha"]
    print(f"  Commit SHA: {commit_sha}")
    print()

    # Step 5: Update ref (master branch) to point to new commit
    print("Step 5: Updating master branch ref...")
    try:
        # Try to update existing ref
        ref_data = {"sha": commit_sha, "force": True}
        ref = api_call(token, "PATCH", f"/repos/{owner}/{repo}/git/refs/heads/master", ref_data)
        print(f"  Updated master -> {commit_sha}")
    except urllib.error.HTTPError as e:
        if e.code == 422:
            # Ref doesn't exist, create it
            print("  master branch doesn't exist, creating...")
            ref_data = {"sha": commit_sha, "ref": "refs/heads/master"}
            ref = api_call(token, "POST", f"/repos/{owner}/{repo}/git/refs", ref_data)
            print(f"  Created master -> {commit_sha}")
        else:
            raise
    print()

    # Step 6: Verify
    print("Step 6: Verifying...")
    repo_info = api_call(token, "GET", f"/repos/{owner}/{repo}")
    print(f"  Repository: {repo_info['full_name']}")
    print(f"  URL: {repo_info['html_url']}")
    print(f"  Default branch: {repo_info.get('default_branch', 'N/A')}")

    # Get commit count
    try:
        commits = api_call(token, "GET", f"/repos/{owner}/{repo}/commits?per_page=5")
        print(f"  Commits: {len(commits)}")
        for c in commits:
            msg = c["commit"]["message"].split("\n")[0][:60]
            print(f"    - {c['sha'][:8]} {msg}")
    except:
        pass

    print()
    print("DONE! Repository uploaded successfully.")

if __name__ == "__main__":
    main()

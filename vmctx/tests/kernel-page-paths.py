#!/usr/bin/env python3
"""Inventory native page boundaries in a frozen kernel source tree.

This is a lexical review aid, not a call graph or an ownership proof. It records
definitions as well as calls, excludes comments/strings, and retains all matches
instead of treating an unrecognized path as covered. Later --overlay trees win.
The file digests make review results attributable to the exact source examined.
"""
import argparse
import bisect
import hashlib
import json
import re
from collections import Counter
from pathlib import Path


PATTERNS = {
    "publish": r"(?:set_(?:ptes|pte_at|pmd_at|pud_at|huge_pte_at)|"
               r"(?:ptep|pmdp|pudp)_set_access_flags|"
               r"(?:vmf|vm)_insert_\w+|remap_pfn_range\w*|"
               r"finish_fault|finish_mkwrite_fault)",
    "revoke": r"(?:(?:ptep|pmdp|pudp)_\w*(?:clear|invalidate)\w*|"
              r"(?:ptep|pmdp|pudp)_set_wrprotect|zap_\w+|"
              r"unmap_(?:mapping|page|single|vmas)\w*)",
    "residency": r"(?:softleaf_\w+|is_\w*(?:swap|migration|device_private|"
                 r"device_exclusive|pte_marker)\w*|"
                 r"make_\w*(?:migration|device_private|device_exclusive)_entry|"
                 r"(?:pte|pmd|pud)_none|migrate_vma_\w+|"
                 r"remove_migration_ptes|restore_exclusive_pte)",
    "identity": r"(?:copy_(?:page_range|pte_range|nonpresent_pte|present_pte|"
                r"huge_pmd|huge_pud)|move_(?:page_tables|ptes|softleaf_pte|"
                r"normal_pmd|normal_pud|huge_pmd|huge_pud)|"
                r"(?:folio|page)_(?:add|remove|move|dup)_\w*rmap\w*)",
    "release": r"(?:(?:folio|page)_(?:put|ref_sub_and_test|ref_dec_and_test)|"
               r"put_page|put_dev_pagemap|free_pages_and_swap_cache|"
               r"free_swap_and_cache|swap_free|swap_duplicate|"
               r"(?:free|release)_\w*(?:folio|page)\w*|folio_free)",
    "pin": r"(?:(?:get|pin)_user_pages\w*|(?:get|pin)_kernel_pages\w*|"
           r"unpin_user_\w+|try_grab_\w+|folio_maybe_dma_pinned|"
           r"(?:folio|page)_ref_freeze)",
    "object": r"(?:(?:filemap|shmem|truncate|invalidate)_\w*(?:folio|page|"
              r"range)\w*|delete_from_page_cache|"
              r"replace_page_cache_folio|folio_mark_dirty|"
              r"folio_start_writeback|folio_end_writeback)",
    "secondary_mmu": r"(?:mmu_(?:interval_)?notifier_\w+|"
                     r"mmu_interval_read_\w+|flush_tlb_\w+|"
                     r"tlb_(?:flush|finish|remove)_\w+)",
}
MATCHERS = {name: re.compile(r"\b(" + pattern + r")\s*\(")
            for name, pattern in PATTERNS.items()}
LITERALS = re.compile(r'/\*.*?\*/|//[^\n]*|"(?:\\.|[^"\\])*"|'
                      r"'(?:\\.|[^'\\])*'", re.S)


def scrub(text):
    # Preserve offsets and line breaks for source links.
    return LITERALS.sub(lambda m: re.sub(r"[^\n]", " ", m.group()), text)


def inventory(roots):
    files = {}
    for root in roots:
        if not root.is_dir():
            raise ValueError(f"source tree missing: {root}")
        for path in sorted(root.rglob("*")):
            rel = path.relative_to(root)
            if (path.is_file() and path.suffix in (".c", ".h", ".S")
                    and not any(part.startswith(".") for part in rel.parts)
                    and "generated" not in rel.parts):
                files[str(rel)] = path
    rows, digests = [], {}
    for rel, path in sorted(files.items()):
        raw = path.read_bytes()
        text = raw.decode("utf-8", errors="replace")
        code = scrub(text)
        starts = [0] + [m.end() for m in re.finditer("\n", code)]
        digests[rel] = dict(sha256=hashlib.sha256(raw).hexdigest(),
                            source=str(path), lines=len(starts))
        lines = text.splitlines()
        for category, pattern in MATCHERS.items():
            for match in pattern.finditer(code):
                line = bisect.bisect_right(starts, match.start())
                rows.append(dict(path=rel, line=line, symbol=match.group(1),
                                 category=category, text=lines[line-1].strip()))
    return dict(schema=1, purpose="lexical inventory; coverage is unproven",
                roots=[str(root) for root in roots], files=digests,
                counts=dict(Counter(row["category"] for row in rows)),
                boundaries=sorted(rows, key=lambda r: (r["path"], r["line"], r["category"])))


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("tree", type=Path)
    parser.add_argument("--overlay", type=Path, action="append", default=[])
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()
    result = inventory([p.resolve() for p in [args.tree] + args.overlay])
    # Refuse to overwrite an earlier audit's evidence.
    with args.output.open("x") as stream:
        json.dump(result, stream, indent=2)
        stream.write("\n")
    print(json.dumps(dict(files=len(result["files"]), **result["counts"])))


if __name__ == "__main__":
    main()

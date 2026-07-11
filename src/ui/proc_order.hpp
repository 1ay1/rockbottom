// ui/proc_order.hpp — turns the raw process list into the ORDERED view the
// table renders: either a flat sorted list or a parent→child tree.
//
// This is the single source of truth for row order. Both the app (selection,
// kill targeting, follow) and the panel renderer consume the SAME ordered
// vector, so a selection index always names the row the user sees — in flat
// mode and in tree mode alike.
//
// Design notes for the tree:
//   • Roots are processes whose parent isn't in the (filtered) set — pid 1,
//     kernel_task, and anything whose parent was filtered out. Orphans surface
//     as roots rather than vanishing.
//   • Children of a node are ordered by the ACTIVE sort key, so the tree still
//     answers "who's the hog" — the busiest child floats to the top of its
//     siblings. Roots are ordered the same way.
//   • Guide glyphs use the box-drawing idiom (│ ├─ └─) with the last child
//     getting the corner, so the shape reads at a glance.
//   • A collapsed node keeps its row but omits its subtree; its descendant
//     count rides the row so you know how much is hidden.

#pragma once

#include "../core/metrics.hpp"
#include "../core/sampler.hpp"   // SortKey

#include <algorithm>
#include <set>
#include <string>
#include <unordered_map>
#include <vector>

namespace rockbottom::ui {

// The fully-ordered process view: parallel arrays, all index-aligned.
struct OrderedProcs {
    std::vector<const ProcInfo*> procs;
    std::vector<std::string>     prefix;     // tree guides ("│  ├─ "); empty in flat mode
    std::vector<bool>            has_kids;   // row heads a subtree
    std::vector<bool>            collapsed;  // row's subtree is folded
    std::vector<int>             hidden;     // descendants hidden under a collapsed row
    // Filter-in-tree: a row kept ONLY because a descendant matched (it doesn't
    // match the filter itself) is CONTEXT — the renderer dims it so the matched
    // leaf's lineage stays legible without pretending the ancestor is a hit.
    std::vector<bool>            context;
    // Rollup: CPU% / RSS bytes summed over each row's WHOLE subtree (self +
    // all descendants). The renderer surfaces these on collapsed rows so a
    // folded parent still shows what its hidden children are costing.
    std::vector<double>          sub_cpu;
    std::vector<double>          sub_mem;
    // FLOW TREE: sib_share is this node's subtree-CPU as a fraction of the
    // BUSIEST sibling's subtree-CPU (0..1) — the "which child is the hog" signal
    // rendered as a weight-gutter bar at every branch. depth is the tree depth
    // (0 = root) so the renderer can heat-grade rails by how deep the flow runs.
    std::vector<double>          sib_share;
    std::vector<int>             depth;
    bool                         tree = false;
};

// Comparator for a sort key + direction. `desc` = the natural "biggest first"
// reading for cpu/mem/io; pid/name flip to ascending as their natural default,
// but the caller still controls direction so ▲/▼ works on every column.
[[nodiscard]] inline bool proc_less(const ProcInfo& a, const ProcInfo& b,
                                    SortKey key, bool desc) {
    auto cmp = [&]() -> bool {
        switch (key) {
            case SortKey::Cpu:  return a.cpu > b.cpu;
            case SortKey::Mem:  return a.rss.value > b.rss.value;
            case SortKey::Io:   return (a.io_read.per_sec + a.io_write.per_sec)
                                     > (b.io_read.per_sec + b.io_write.per_sec);
            case SortKey::Pid:  return a.pid < b.pid;
            case SortKey::Name: return a.name < b.name;
            case SortKey::Port: {
                const bool ha = !a.ports.empty(), hb = !b.ports.empty();
                if (ha != hb) return ha;
                if (ha && a.ports.front() != b.ports.front())
                    return a.ports.front() < b.ports.front();
                return a.cpu > b.cpu;
            }
        }
        return a.cpu > b.cpu;
    };
    // cmp() encodes the DESCENDING intent for magnitude keys and ASCENDING for
    // pid/name. `desc` toggles that baseline.
    const bool base = cmp();
    const bool magnitude = key == SortKey::Cpu || key == SortKey::Mem ||
                           key == SortKey::Io || key == SortKey::Port;
    // For magnitude keys base==true means a>b (already "desc"); for pid/name
    // base==true means a<b ("asc"). Normalize so `desc` means the intuitive
    // "biggest / Z-last first", then flip when the user asked for the reverse.
    const bool desc_order = magnitude ? base : !base;
    return desc ? desc_order : !desc_order;
}

// Build the flat sorted list (filter applied by the caller).
inline OrderedProcs order_flat(std::vector<const ProcInfo*> procs,
                               SortKey key, bool desc) {
    std::stable_sort(procs.begin(), procs.end(),
                     [&](const ProcInfo* a, const ProcInfo* b) {
                         return proc_less(*a, *b, key, desc);
                     });
    OrderedProcs out;
    out.procs = std::move(procs);
    out.tree = false;
    return out;
}

// Build the parent→child tree. `all` is the FULL process list; `filter` (name
// or pid substring) selects the MATCH set but the tree keeps every ancestor of
// a match so the matched leaf's lineage stays intact — broot's signature move.
// Ancestors kept only for context are flagged so the renderer can dim them.
// `collapsed` holds pids whose subtree is folded. The active sort orders
// siblings so the tree still ranks by the hot column. Each row also carries its
// subtree CPU%/RSS rollup so collapsed parents show what they're hiding.
inline OrderedProcs order_tree(const std::vector<const ProcInfo*>& all,
                               const std::string& filter,
                               SortKey key, bool desc,
                               const std::set<int>& collapsed) {
    OrderedProcs out;
    out.tree = true;

    // Index by pid and bucket children under their parent.
    std::unordered_map<int, const ProcInfo*> by_pid;
    by_pid.reserve(all.size() * 2);
    for (const ProcInfo* p : all) by_pid[p->pid] = p;

    // Match predicate: empty filter matches everything.
    auto matches = [&](const ProcInfo* p) {
        return filter.empty() ||
               p->name.find(filter) != std::string::npos ||
               std::to_string(p->pid).find(filter) != std::string::npos;
    };

    // Keep-set: every match PLUS every ancestor of a match (walk ppid links up
    // through the pid index). `is_match` distinguishes real hits from the
    // ancestor rows kept only as context. With no filter, keep == all.
    std::unordered_map<int, bool> is_match;   // pid -> matched the filter itself
    std::set<int> keep;
    if (filter.empty()) {
        for (const ProcInfo* p : all) { keep.insert(p->pid); is_match[p->pid] = true; }
    } else {
        for (const ProcInfo* p : all) {
            if (!matches(p)) continue;
            is_match[p->pid] = true;
            // Walk up, adding ancestors as context (unless they too matched).
            int pid = p->pid;
            while (keep.insert(pid).second || pid == p->pid) {
                const ProcInfo* node = by_pid.count(pid) ? by_pid[pid] : nullptr;
                if (pid != p->pid) is_match.try_emplace(pid, false);
                if (!node || node->ppid <= 0 || node->ppid == node->pid
                    || !by_pid.count(node->ppid)) break;
                pid = node->ppid;
            }
        }
    }

    // Restrict the working set to kept nodes.
    std::vector<const ProcInfo*> visible;
    visible.reserve(keep.size());
    for (const ProcInfo* p : all)
        if (keep.count(p->pid)) visible.push_back(p);

    std::unordered_map<int, std::vector<const ProcInfo*>> kids;
    std::vector<const ProcInfo*> roots;
    for (const ProcInfo* p : visible) {
        const bool parent_here = p->ppid > 0 && keep.count(p->ppid) && p->ppid != p->pid;
        if (parent_here) kids[p->ppid].push_back(p);
        else             roots.push_back(p);   // pid1 / orphan / filtered-out parent
    }

    auto sib_sort = [&](std::vector<const ProcInfo*>& v) {
        std::stable_sort(v.begin(), v.end(),
                         [&](const ProcInfo* a, const ProcInfo* b) {
                             return proc_less(*a, *b, key, desc);
                         });
    };
    sib_sort(roots);
    for (auto& [_, v] : kids) sib_sort(v);

    // Post-order DFS: fill descendant count AND the CPU/MEM subtree rollups in
    // one pass (self + children's already-computed sums).
    std::unordered_map<int, int>    subtree_n;
    std::unordered_map<int, double> sub_cpu, sub_mem;
    {
        std::vector<std::pair<const ProcInfo*, bool>> st;
        for (auto it = roots.rbegin(); it != roots.rend(); ++it) st.push_back({*it, false});
        std::vector<const ProcInfo*> post;
        while (!st.empty()) {
            auto [node, done] = st.back(); st.pop_back();
            if (done) { post.push_back(node); continue; }
            st.push_back({node, true});
            if (auto k = kids.find(node->pid); k != kids.end())
                for (const ProcInfo* c : k->second) st.push_back({c, false});
        }
        for (const ProcInfo* node : post) {
            int n = 0;
            double cpu = node->cpu;
            double mem = static_cast<double>(node->rss.value);
            if (auto k = kids.find(node->pid); k != kids.end())
                for (const ProcInfo* c : k->second) {
                    n   += 1 + subtree_n[c->pid];
                    cpu += sub_cpu[c->pid];
                    mem += sub_mem[c->pid];
                }
            subtree_n[node->pid] = n;
            sub_cpu[node->pid]   = cpu;
            sub_mem[node->pid]   = mem;
        }
    }

    // FLOW TREE: for each sibling group (roots + each parent's kids) find the
    // busiest subtree so a node's bar reads as its share of the heaviest
    // sibling — the dominant child at every branch is full-height.
    std::unordered_map<int, double> group_max_cpu;   // keyed by parent pid (0 = roots)
    {
        double rmax = 0;
        for (const ProcInfo* r : roots) rmax = std::max(rmax, sub_cpu[r->pid]);
        group_max_cpu[0] = rmax;
        for (auto& [ppid, v] : kids) {
            double mx = 0;
            for (const ProcInfo* c : v) mx = std::max(mx, sub_cpu[c->pid]);
            group_max_cpu[ppid] = mx;
        }
    }

    // Pre-order emit with an explicit stack, carrying the running prefix and
    // whether each node is the last of its siblings (for └─ vs ├─).
    struct Frame { const ProcInfo* node; std::string prefix; bool last; int depth; int parent; };
    std::vector<Frame> st;
    for (std::size_t i = roots.size(); i-- > 0;)
        st.push_back({roots[i], "", i + 1 == roots.size(), 0, 0});

    while (!st.empty()) {
        Frame f = std::move(st.back()); st.pop_back();
        const ProcInfo* node = f.node;
        auto k = kids.find(node->pid);
        const bool kids_exist = k != kids.end() && !k->second.empty();
        const bool is_collapsed = collapsed.count(node->pid) > 0;

        // Row prefix: at depth 0 there's no guide; deeper rows get the parent
        // rails already accumulated in f.prefix plus this node's connector.
        std::string row_prefix;
        if (f.depth > 0)
            row_prefix = f.prefix + (f.last ? "└─ " : "├─ ");

        // Sibling share: this subtree's cpu vs the busiest sibling's (0..1).
        double gmax = group_max_cpu.count(f.parent) ? group_max_cpu[f.parent] : 0;
        double share = gmax > 0.01 ? std::clamp(sub_cpu[node->pid] / gmax, 0.0, 1.0) : 0.0;

        out.procs.push_back(node);
        out.prefix.push_back(row_prefix);
        out.has_kids.push_back(kids_exist);
        out.collapsed.push_back(kids_exist && is_collapsed);
        out.hidden.push_back(kids_exist && is_collapsed ? subtree_n[node->pid] : 0);
        out.context.push_back(!is_match[node->pid]);
        out.sub_cpu.push_back(sub_cpu[node->pid]);
        out.sub_mem.push_back(sub_mem[node->pid]);
        out.sib_share.push_back(share);
        out.depth.push_back(f.depth);

        if (kids_exist && !is_collapsed) {
            // Children inherit this node's rail: a vertical bar if we're not
            // the last sibling, blank space if we are (so the tree "closes").
            std::string child_prefix = f.depth > 0
                ? f.prefix + (f.last ? "   " : "│  ")
                : "";
            const auto& cv = k->second;
            for (std::size_t i = cv.size(); i-- > 0;)
                st.push_back({cv[i], child_prefix, i + 1 == cv.size(), f.depth + 1, node->pid});
        }
    }

    return out;
}

// Top-level: filter → order. `filter` matches name OR pid substring. Flat mode
// pre-filters to matches; tree mode receives the FULL list + filter and does
// its own ancestor-preserving keep pass (so a matched leaf keeps its lineage).
inline OrderedProcs order_procs(const std::vector<ProcInfo>& all,
                                const std::string& filter,
                                SortKey key, bool desc, bool tree,
                                const std::set<int>& collapsed) {
    if (tree) {
        std::vector<const ProcInfo*> ptrs;
        ptrs.reserve(all.size());
        for (const auto& p : all) ptrs.push_back(&p);
        return order_tree(ptrs, filter, key, desc, collapsed);
    }
    std::vector<const ProcInfo*> vis;
    vis.reserve(all.size());
    for (const auto& p : all) {
        if (filter.empty() ||
            p.name.find(filter) != std::string::npos ||
            std::to_string(p.pid).find(filter) != std::string::npos)
            vis.push_back(&p);
    }
    return order_flat(std::move(vis), key, desc);
}

}  // namespace rockbottom::ui

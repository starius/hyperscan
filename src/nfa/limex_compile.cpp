/*
 * Copyright (c) 2015-2016, Intel Corporation
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are met:
 *
 *  * Redistributions of source code must retain the above copyright notice,
 *    this list of conditions and the following disclaimer.
 *  * Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 *  * Neither the name of Intel Corporation nor the names of its contributors
 *    may be used to endorse or promote products derived from this software
 *    without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
 * AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT OWNER OR CONTRIBUTORS BE
 * LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
 * CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
 * SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
 * INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
 * CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
 * ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
 * POSSIBILITY OF SUCH DAMAGE.
 */

/** \file
 * \brief Main NFA build code.
 */
#include "limex_compile.h"

#include "accel.h"
#include "accelcompile.h"
#include "grey.h"
#include "limex_internal.h"
#include "limex_limits.h"
#include "nfa_build_util.h"
#include "nfagraph/ng_holder.h"
#include "nfagraph/ng_limex_accel.h"
#include "nfagraph/ng_repeat.h"
#include "nfagraph/ng_restructuring.h"
#include "nfagraph/ng_squash.h"
#include "nfagraph/ng_util.h"
#include "ue2common.h"
#include "repeatcompile.h"
#include "util/alloc.h"
#include "util/bitutils.h"
#include "util/charreach.h"
#include "util/compile_context.h"
#include "util/container.h"
#include "util/graph.h"
#include "util/graph_range.h"
#include "util/order_check.h"
#include "util/verify_types.h"
#include "util/ue2_containers.h"

#include <algorithm>
#include <cassert>
#include <cstddef>
#include <cstdlib>
#include <cstring>
#include <map>
#include <set>
#include <vector>
#include <boost/graph/breadth_first_search.hpp>

using namespace std;

namespace ue2 {

namespace {

struct precalcAccel {
    precalcAccel() : single_offset(0), double_offset(0) {}
    CharReach single_cr;
    u32 single_offset;

    CharReach double_cr;
    flat_set<pair<u8, u8>> double_lits; /* double-byte accel stop literals */
    u32 double_offset;

    MultibyteAccelInfo ma_info;
};

struct limex_accel_info {
    ue2::unordered_set<NFAVertex> accelerable;
    map<NFAStateSet, precalcAccel> precalc;
    ue2::unordered_map<NFAVertex, flat_set<NFAVertex> > friends;
    ue2::unordered_map<NFAVertex, AccelScheme> accel_map;
};

static
map<NFAVertex, NFAStateSet>
reindexByStateId(const map<NFAVertex, NFAStateSet> &in, const NGHolder &g,
                 const ue2::unordered_map<NFAVertex, u32> &state_ids,
                 const u32 num_states) {
    map<NFAVertex, NFAStateSet> out;

    vector<u32> indexToState(num_vertices(g), NO_STATE);
    for (const auto &m : state_ids) {
        u32 vert_id = g[m.first].index;
        assert(vert_id < indexToState.size());
        indexToState[vert_id] = m.second;
    }

    for (const auto &m : in) {
        NFAVertex v = m.first;
        assert(m.second.size() <= indexToState.size());

        NFAStateSet mask(num_states);
        for (size_t i = m.second.find_first(); i != m.second.npos;
             i = m.second.find_next(i)) {
            u32 state_id = indexToState[i];
            if (state_id == NO_STATE) {
                continue;
            }
            mask.set(state_id);
        }
        out.emplace(v, mask);
    }

    return out;
}

struct build_info {
    build_info(NGHolder &hi,
               const ue2::unordered_map<NFAVertex, u32> &states_in,
               const vector<BoundedRepeatData> &ri,
               const map<NFAVertex, NFAStateSet> &rsmi,
               const map<NFAVertex, NFAStateSet> &smi,
               const map<u32, NFAVertex> &ti, const set<NFAVertex> &zi,
               bool dai, bool sci, const CompileContext &cci,
               u32 nsi)
        : h(hi), state_ids(states_in), repeats(ri), tops(ti), zombies(zi),
          do_accel(dai), stateCompression(sci), cc(cci),
          num_states(nsi) {
        for (const auto &br : repeats) {
            insert(&tugs, br.tug_triggers);
            br_cyclic[br.cyclic] =
                BoundedRepeatSummary(br.repeatMin, br.repeatMax);
        }

        // Convert squash maps to be indexed by state index rather than
        // vertex_index.
        squashMap = reindexByStateId(smi, h, state_ids, num_states);
        reportSquashMap = reindexByStateId(rsmi, h, state_ids, num_states);
    }

    NGHolder &h;
    const ue2::unordered_map<NFAVertex, u32> &state_ids;
    const vector<BoundedRepeatData> &repeats;

    // Squash maps; state sets are indexed by state_id.
    map<NFAVertex, NFAStateSet> reportSquashMap;
    map<NFAVertex, NFAStateSet> squashMap;

    const map<u32, NFAVertex> &tops;
    ue2::unordered_set<NFAVertex> tugs;
    map<NFAVertex, BoundedRepeatSummary> br_cyclic;
    const set<NFAVertex> &zombies;
    bool do_accel;
    bool stateCompression;
    const CompileContext &cc;
    u32 num_states;
    limex_accel_info accel;
};

#define LAST_LIMEX_NFA LIMEX_NFA_512

// Constants for scoring mechanism
const int SHIFT_COST = 10; // limex: cost per shift mask
const int EXCEPTION_COST = 4; // limex: per exception

template<NFAEngineType t> struct NFATraits { };

template<template<NFAEngineType t> class sfunc, typename rv_t, typename arg_t,
         NFAEngineType lb>
struct DISPATCH_BY_LIMEX_TYPE_INT {
    static rv_t doOp(NFAEngineType i, const arg_t &arg) {
        if (i == lb) {
            return sfunc<lb>::call(arg);
        } else {
            return DISPATCH_BY_LIMEX_TYPE_INT<sfunc, rv_t, arg_t,
                                            (NFAEngineType)(lb + 1)>
                     ::doOp(i, arg);
        }
    }
};

template<template<NFAEngineType t> class sfunc, typename rv_t, typename arg_t>
struct DISPATCH_BY_LIMEX_TYPE_INT<sfunc, rv_t, arg_t,
                                  (NFAEngineType)(LAST_LIMEX_NFA + 1)> {
    // dummy
    static rv_t doOp(NFAEngineType, const arg_t &) {
        assert(0);
        throw std::logic_error("Unreachable");
    }
};

#define DISPATCH_BY_LIMEX_TYPE(i, op, arg)                                     \
    DISPATCH_BY_LIMEX_TYPE_INT<op, decltype(op<(NFAEngineType)0>::call(arg)),  \
                               decltype(arg), (NFAEngineType)0>::doOp(i, arg)

// Given a number of states, find the size of the smallest container NFA it
// will fit in. We support NFAs of the following sizes: 32, 64, 128, 256, 384,
// 512.
size_t findContainerSize(size_t states) {
    if (states > 256 && states <= 384) {
        return 384;
    }
    return 1ULL << (lg2(states - 1) + 1);
}

bool isLimitedTransition(int from, int to, int maxshift) {
    int diff = to - from;

    // within our shift?
    if (diff < 0 || diff > maxshift) {
        return false;
    }

    // can't jump over a bollard
    return (from & ~63) == (to & ~63);
}

// Fill a bit mask
template<class Mask>
void maskFill(Mask &m, char c) {
    memset(&m, c, sizeof(m));
}

// Clear a bit mask.
template<class Mask>
void maskClear(Mask &m) {
    memset(&m, 0, sizeof(m));
}

template<class Mask>
u8 *maskGetByte(Mask &m, u32 bit) {
    assert(bit < sizeof(m)*8);
    u8 *m8 = (u8 *)&m;

    return m8 + bit/8;
}

// Set a bit in a mask, starting from the little end.
template<class Mask>
void maskSetBit(Mask &m, const unsigned int bit) {
    u8 *byte = maskGetByte(m, bit);
    *byte |= 1U << (bit % 8);
}

template<class Mask>
void maskSetBits(Mask &m, const NFAStateSet &bits) {
    for (size_t i = bits.find_first(); i != bits.npos; i = bits.find_next(i)) {
        maskSetBit(m, i);
    }
}

template<class Mask>
bool isMaskZero(Mask &m) {
    u8 *m8 = (u8 *)&m;
    for (u32 i = 0; i < sizeof(m); i++) {
        if (m8[i]) {
            return false;
        }
    }
    return true;
}

// Sets an entire byte in a mask to the given value
template<class Mask>
void maskSetByte(Mask &m, const unsigned int idx, const char val) {
    assert(idx < sizeof(m));
    char *m8 = (char *)&m;
    char &byte = m8[idx];
    byte = val;
}

// Clear a bit in the mask, starting from the little end.
template<class Mask>
void maskClearBit(Mask &m, const u32 bit) {
    u8 *byte = maskGetByte(m, bit);
    *byte &= ~(1U << (bit % 8));
}

/*
 * Common code: the following code operates on parts of the NFA that are common
 * to both the (defunct) General and the LimEx models.
 */

static
void buildReachMapping(const build_info &args, vector<NFAStateSet> &reach,
                       vector<u8> &reachMap) {
    const NGHolder &h = args.h;
    const auto &state_ids = args.state_ids;

    // Build a list of vertices with a state index assigned.
    vector<NFAVertex> verts;
    verts.reserve(args.num_states);
    for (auto v : vertices_range(h)) {
        if (state_ids.at(v) != NO_STATE) {
            verts.push_back(v);
        }
    }

    // Build a mapping from set-of-states -> reachability.
    map<NFAStateSet, CharReach> mapping;
    NFAStateSet states(args.num_states);
    for (size_t i = 0; i < N_CHARS; i++) {
        states.reset();
        for (auto v : verts) {
            const CharReach &cr = h[v].char_reach;
            if (cr.test(i)) {
                u32 state_id = state_ids.at(v);
                states.set(state_id);
            }
        }
        mapping[states].set(i);
    }

    DEBUG_PRINTF("%zu distinct reachability entries\n", mapping.size());
    assert(!mapping.empty());

    // Build a vector of distinct reachability entries and a mapping from every
    // character to one of those entries.

    reach.reserve(mapping.size());
    reachMap.assign(N_CHARS, 0);

    u8 num = 0;
    for (auto mi = mapping.begin(), me = mapping.end(); mi != me; ++mi, ++num) {
        // Reach entry.
        reach.push_back(mi->first);

        // Character mapping.
        const CharReach &cr = mi->second;
        for (size_t i = cr.find_first(); i != CharReach::npos;
             i = cr.find_next(i)) {
            reachMap[i] = num;
        }
    }
}

struct AccelBuild {
    AccelBuild() : v(NFAGraph::null_vertex()), state(0), offset(0), ma_len1(0),
            ma_len2(0), ma_type(MultibyteAccelInfo::MAT_NONE) {}
    NFAVertex v;
    u32 state;
    u32 offset; // offset correction to apply
    CharReach stop1; // single-byte accel stop literals
    flat_set<pair<u8, u8>> stop2; // double-byte accel stop literals
    u32 ma_len1; // multiaccel len1
    u32 ma_len2; // multiaccel len2
    MultibyteAccelInfo::multiaccel_type ma_type; // multiaccel type
};

static
void findStopLiterals(const build_info &bi, NFAVertex v, AccelBuild &build) {
    u32 state = bi.state_ids.at(v);
    build.v = v;
    build.state = state;
    NFAStateSet ss(bi.num_states);
    ss.set(state);

    if (!contains(bi.accel.precalc, ss)) {
        build.stop1 = CharReach::dot();
    } else {
        const precalcAccel &precalc = bi.accel.precalc.at(ss);
        unsigned ma_len = precalc.ma_info.len1 + precalc.ma_info.len2;
        if (ma_len >= MULTIACCEL_MIN_LEN) {
            build.ma_len1 = precalc.ma_info.len1;
            build.stop1 = precalc.ma_info.cr;
            build.offset = precalc.ma_info.offset;
        } else if (precalc.double_lits.empty()) {
            build.stop1 = precalc.single_cr;
            build.offset = precalc.single_offset;
        } else {
            build.stop1 = precalc.double_cr;
            build.stop2 = precalc.double_lits;
            build.offset = precalc.double_offset;
        }
    }

#ifdef DEBUG
    printf("state %u stop1:", state);
    for (size_t j = build.stop1.find_first(); j != build.stop1.npos;
         j = build.stop1.find_next(j)) {
        printf(" 0x%02x", (u32)j);
    }
    printf("\n");
    printf("state %u stop2:", state);
    for (auto it = build.stop2.begin(); it != build.stop2.end(); ++it) {
        printf(" 0x%02hhx%02hhx", it->first, it->second);
    }
    printf("\n");
#endif
}

// Generate all the data we need for at most NFA_MAX_ACCEL_STATES accelerable
// states.
static
void gatherAccelStates(const build_info &bi, vector<AccelBuild> &accelStates) {
    for (auto v : bi.accel.accelerable) {
        DEBUG_PRINTF("state %u is accelerable\n", bi.state_ids.at(v));
        AccelBuild a;
        findStopLiterals(bi, v, a);
        accelStates.push_back(a);
    }

    // AccelStates should be sorted by state number, so that we build our accel
    // masks correctly.
    sort(accelStates.begin(), accelStates.end(),
         [](const AccelBuild &a, const AccelBuild &b) {
             return a.state < b.state;
         });

    // Our caller shouldn't have fed us too many accel states.
    assert(accelStates.size() <= NFA_MAX_ACCEL_STATES);
    if (accelStates.size() > NFA_MAX_ACCEL_STATES) {
        accelStates.resize(NFA_MAX_ACCEL_STATES);
    }
}

static
void combineAccel(const AccelBuild &in, AccelBuild &out) {
    // stop1 and stop2 union
    out.stop1 |= in.stop1;
    out.stop2.insert(in.stop2.begin(), in.stop2.end());
    // offset is maximum of the two
    out.offset = max(out.offset, in.offset);
}

static
void minimiseAccel(AccelBuild &build) {
    flat_set<pair<u8, u8>> new_stop2;
    // Any two-byte accels beginning with a one-byte accel should be removed
    for (const auto &si : build.stop2) {
        if (!build.stop1.test(si.first)) {
            new_stop2.insert(si);
        }
    }
    build.stop2 = new_stop2;
}

struct AccelAuxCmp {
    explicit AccelAuxCmp(const AccelAux &aux_in) : aux(aux_in) {}
    bool operator()(const AccelAux &a) const {
        return !memcmp(&a, &aux, sizeof(AccelAux));
    }
private:
    const AccelAux &aux;
};

static
bool allow_wide_accel(NFAVertex v, const NGHolder &g, NFAVertex sds_or_proxy) {
    return v == sds_or_proxy || edge(g.start, v, g).second;
}

static
bool allow_wide_accel(const vector<NFAVertex> &vv, const NGHolder &g,
                      NFAVertex sds_or_proxy) {
    for (auto v : vv) {
        if (allow_wide_accel(v, g, sds_or_proxy)) {
            return true;
        }
    }

    return false;
}

// identify and mark states that we feel are accelerable (for a limex NFA)
/* Note: leftfix nfas allow accepts to be accelerated */
static
void nfaFindAccelSchemes(const NGHolder &g,
                         const map<NFAVertex, BoundedRepeatSummary> &br_cyclic,
                         ue2::unordered_map<NFAVertex, AccelScheme> *out) {
    vector<CharReach> refined_cr = reduced_cr(g, br_cyclic);

    NFAVertex sds_or_proxy = get_sds_or_proxy(g);

    for (auto v : vertices_range(g)) {
        // We want to skip any vertices that don't lead to at least one other
        // (self-loops don't count) vertex.
        if (!has_proper_successor(v, g)) {
            DEBUG_PRINTF("skipping vertex %u\n", g[v].index);
            continue;
        }

        bool allow_wide = allow_wide_accel(v, g, sds_or_proxy);

        AccelScheme as;
        if (nfaCheckAccel(g, v, refined_cr, br_cyclic, &as, allow_wide)) {
            DEBUG_PRINTF("graph vertex %u is accelerable with offset %u.\n",
                          g[v].index, as.offset);
            (*out)[v] = as;
        }
    }
}

struct fas_visitor : public boost::default_bfs_visitor {
    fas_visitor(const ue2::unordered_map<NFAVertex, AccelScheme> &am_in,
                ue2::unordered_map<NFAVertex, AccelScheme> *out_in)
        : accel_map(am_in), out(out_in) {}

    void discover_vertex(NFAVertex v, const NFAGraph &) {
        if (accel_map.find(v) != accel_map.end()) {
            (*out)[v] = accel_map.find(v)->second;
        }
        if (out->size() >= NFA_MAX_ACCEL_STATES) {
            throw this; /* done */
        }
    }
    const ue2::unordered_map<NFAVertex, AccelScheme> &accel_map;
    ue2::unordered_map<NFAVertex, AccelScheme> *out;
};

static
void filterAccelStates(NGHolder &g, const map<u32, NFAVertex> &tops,
                       ue2::unordered_map<NFAVertex, AccelScheme> *accel_map) {
    /* We want the NFA_MAX_ACCEL_STATES best acceleration states, everything
     * else should be ditched. We use a simple BFS to choose accel states near
     * the start. */

    // Temporarily wire start to each top for the BFS.
    vector<NFAEdge> topEdges;
    wireStartToTops(g, tops, topEdges);

    // Similarly, connect (start, startDs) if necessary.
    if (!edge(g.start, g.startDs, g).second) {
        auto e = add_edge(g.start, g.startDs, g).first;
        topEdges.push_back(e); // Remove edge later.
    }

    ue2::unordered_map<NFAVertex, AccelScheme> out;

    try {
        vector<boost::default_color_type> colour(num_vertices(g));
        breadth_first_search(
            g.g, g.start,
            visitor(fas_visitor(*accel_map, &out))
                .color_map(make_iterator_property_map(
                    colour.begin(), get(&NFAGraphVertexProps::index, g.g))));
    } catch (fas_visitor *) {
        ; /* found max accel_states */
    }

    remove_edges(topEdges, g);

    assert(out.size() <= NFA_MAX_ACCEL_STATES);
    accel_map->swap(out);
}

static
bool containsBadSubset(const limex_accel_info &accel,
                       const NFAStateSet &state_set, const u32 effective_sds) {
    NFAStateSet subset(state_set.size());
    for (size_t j = state_set.find_first(); j != state_set.npos;
         j = state_set.find_next(j)) {
        subset = state_set;
        subset.reset(j);

        if (effective_sds != NO_STATE && subset.count() == 1 &&
            subset.test(effective_sds)) {
            continue;
        }

        if (subset.any() && !contains(accel.precalc, subset)) {
            return true;
        }
    }
    return false;
}

static
bool is_too_wide(const AccelScheme &as) {
    return as.cr.count() > MAX_MERGED_ACCEL_STOPS;
}

static
void fillAccelInfo(build_info &bi) {
    if (!bi.do_accel) {
        return;
    }

    NGHolder &g = bi.h;
    limex_accel_info &accel = bi.accel;
    unordered_map<NFAVertex, AccelScheme> &accel_map = accel.accel_map;
    const map<NFAVertex, BoundedRepeatSummary> &br_cyclic = bi.br_cyclic;
    const CompileContext &cc = bi.cc;
    const unordered_map<NFAVertex, u32> &state_ids = bi.state_ids;
    const u32 num_states = bi.num_states;

    nfaFindAccelSchemes(g, br_cyclic, &accel_map);
    filterAccelStates(g, bi.tops, &accel_map);

    assert(accel_map.size() <= NFA_MAX_ACCEL_STATES);

    vector<CharReach> refined_cr = reduced_cr(g, br_cyclic);

    vector<NFAVertex> astates;
    for (const auto &m : accel_map) {
        astates.push_back(m.first);
    }

    NFAStateSet useful(num_states);
    NFAStateSet state_set(num_states);
    vector<NFAVertex> states;

    NFAVertex sds_or_proxy = get_sds_or_proxy(g);
    const u32 effective_sds = state_ids.at(sds_or_proxy);

    /* for each subset of the accel keys need to find an accel scheme */
    assert(astates.size() < 32);
    sort(astates.begin(), astates.end(), make_index_ordering(g));

    for (u32 i = 1, i_end = 1U << astates.size(); i < i_end; i++) {
        DEBUG_PRINTF("saving info for accel %u\n", i);
        states.clear();
        state_set.reset();
        for (u32 j = 0, j_end = astates.size(); j < j_end; j++) {
            if (i & (1U << j)) {
                NFAVertex v = astates[j];
                states.push_back(v);
                state_set.set(state_ids.at(v));
            }
        }

        if (containsBadSubset(accel, state_set, effective_sds)) {
            DEBUG_PRINTF("accel %u has bad subset\n", i);
            continue; /* if a subset failed to build we would too */
        }

        const bool allow_wide = allow_wide_accel(states, g, sds_or_proxy);

        AccelScheme as = nfaFindAccel(g, states, refined_cr, br_cyclic,
                                      allow_wide, true);
        if (is_too_wide(as)) {
            DEBUG_PRINTF("accel %u too wide (%zu, %d)\n", i,
                         as.cr.count(), MAX_MERGED_ACCEL_STOPS);
            continue;
        }

        DEBUG_PRINTF("accel %u ok with offset s%u, d%u\n", i, as.offset,
                     as.double_offset);

        // try multibyte acceleration first
        MultibyteAccelInfo mai = nfaCheckMultiAccel(g, states, cc);

        precalcAccel &pa = accel.precalc[state_set];
        useful |= state_set;

        // if we successfully built a multibyte accel scheme, use that
        if (mai.type != MultibyteAccelInfo::MAT_NONE) {
            pa.ma_info = mai;

            DEBUG_PRINTF("multibyte acceleration!\n");
            continue;
        }

        pa.single_offset = as.offset;
        pa.single_cr = as.cr;
        if (as.double_byte.size() != 0) {
            pa.double_offset = as.double_offset;
            pa.double_lits = as.double_byte;
            pa.double_cr = as.double_cr;
        };
    }

    for (const auto &m : accel_map) {
        NFAVertex v = m.first;
        const u32 state_id = state_ids.at(v);

        /* if we we unable to make a scheme out of the state in any context,
         * there is not point marking it as accelerable */
        if (!useful.test(state_id)) {
            continue;
        }

        u32 offset = 0;
        state_set.reset();
        state_set.set(state_id);

        bool is_multi = false;
        auto p_it = accel.precalc.find(state_set);
        if (p_it != accel.precalc.end()) {
            const precalcAccel &pa = p_it->second;
            offset = max(pa.double_offset, pa.single_offset);
            is_multi = pa.ma_info.type != MultibyteAccelInfo::MAT_NONE;
            assert(offset <= MAX_ACCEL_DEPTH);
        }

        accel.accelerable.insert(v);
        if (!is_multi) {
            findAccelFriends(g, v, br_cyclic, offset, &accel.friends[v]);
        }
    }
}

/** The AccelAux structure has large alignment specified, and this makes some
 * compilers do odd things unless we specify a custom allocator. */
typedef vector<AccelAux, AlignedAllocator<AccelAux, alignof(AccelAux)> >
    AccelAuxVector;

static
void buildAccel(const build_info &args, NFAStateSet &accelMask,
                NFAStateSet &accelFriendsMask, AccelAuxVector &auxvec,
                vector<u8> &accelTable) {
    const limex_accel_info &accel = args.accel;

    // Init, all zeroes.
    accelMask.resize(args.num_states);
    accelFriendsMask.resize(args.num_states);

    if (!args.do_accel) {
        return;
    }

    vector<AccelBuild> accelStates;
    gatherAccelStates(args, accelStates);

    if (accelStates.empty()) {
        DEBUG_PRINTF("no accelerable states\n");
        return;
    }

    // We have 2^n different accel entries, one for each possible
    // combination of accelerable states.
    assert(accelStates.size() < 32);
    const u32 accelCount = 1U << accelStates.size();
    assert(accelCount <= 256);

    // Set up a unioned AccelBuild for every possible combination of the set
    // bits in accelStates.
    vector<AccelBuild> accelOuts(accelCount);
    for (u32 i = 1; i < accelCount; i++) {
        for (u32 j = 0, j_end = accelStates.size(); j < j_end; j++) {
            if (i & (1U << j)) {
                combineAccel(accelStates[j], accelOuts[i]);
            }
        }
        minimiseAccel(accelOuts[i]);
    }

    accelTable.resize(accelCount);

    // We dedupe our AccelAux structures here, so that we only write one copy
    // of each unique accel scheme into the bytecode, using the accelTable as
    // an index.

    // Start with the NONE case.
    auxvec.push_back(AccelAux());
    memset(&auxvec[0], 0, sizeof(AccelAux));
    auxvec[0].accel_type = ACCEL_NONE; // no states on.

    AccelAux aux;
    for (u32 i = 1; i < accelCount; i++) {
        memset(&aux, 0, sizeof(aux));

        NFAStateSet states(args.num_states);
        for (u32 j = 0; j < accelStates.size(); j++) {
            if (i & (1U << j)) {
                states.set(accelStates[j].state);
            }
        }

        AccelInfo ainfo;
        ainfo.double_offset = accelOuts[i].offset;
        ainfo.double_stop1 = accelOuts[i].stop1;
        ainfo.double_stop2 = accelOuts[i].stop2;

        if (contains(accel.precalc, states)) {
            const precalcAccel &precalc = accel.precalc.at(states);
            if (precalc.ma_info.type != MultibyteAccelInfo::MAT_NONE) {
                ainfo.ma_len1 = precalc.ma_info.len1;
                ainfo.ma_len2 = precalc.ma_info.len2;
                ainfo.multiaccel_offset = precalc.ma_info.offset;
                ainfo.multiaccel_stops = precalc.ma_info.cr;
                ainfo.ma_type = precalc.ma_info.type;
            } else {
                ainfo.single_offset = precalc.single_offset;
                ainfo.single_stops = precalc.single_cr;
            }
        }

        buildAccelAux(ainfo, &aux);

        // FIXME: We may want a faster way to find AccelAux structures that
        // we've already built before.
        auto it = find_if(auxvec.begin(), auxvec.end(), AccelAuxCmp(aux));
        if (it == auxvec.end()) {
            accelTable[i] = verify_u8(auxvec.size());
            auxvec.push_back(aux);
        } else {
            accelTable[i] = verify_u8(it - auxvec.begin());
        }
    }

    DEBUG_PRINTF("%zu unique accel schemes (of max %u)\n", auxvec.size(),
                 accelCount);

    // XXX: ACCEL_NONE?
    for (const auto &as : accelStates) {
        NFAVertex v = as.v;
        assert(v && args.state_ids.at(v) == as.state);

        accelMask.set(as.state);
        accelFriendsMask.set(as.state);

        if (!contains(accel.friends, v)) {
            continue;
        }
        // Add the friends of this state to the friends mask.
        const flat_set<NFAVertex> &friends = accel.friends.at(v);
        DEBUG_PRINTF("%u has %zu friends\n", as.state, friends.size());
        for (auto friend_v : friends) {
            u32 state_id = args.state_ids.at(friend_v);
            DEBUG_PRINTF("--> %u\n", state_id);
            accelFriendsMask.set(state_id);
        }
    }
}

static
void buildAccepts(const build_info &args, NFAStateSet &acceptMask,
                  NFAStateSet &acceptEodMask, vector<NFAAccept> &accepts,
                  vector<NFAAccept> &acceptsEod, vector<NFAStateSet> &squash) {
    const NGHolder &h = args.h;

    acceptMask.resize(args.num_states);
    acceptEodMask.resize(args.num_states);

    for (auto v : vertices_range(h)) {
        u32 state_id = args.state_ids.at(v);

        if (state_id == NO_STATE || !is_match_vertex(v, h)) {
            continue;
        }

        u32 squashMaskOffset = MO_INVALID_IDX;
        auto sit = args.reportSquashMap.find(v);
        if (sit != args.reportSquashMap.end()) {
            // This state has a squash mask. Paw through the existing vector to
            // see if we've already seen it, otherwise add a new one.
            auto it = find(squash.begin(), squash.end(), sit->second);
            if (it != squash.end()) {
                squashMaskOffset = verify_u32(distance(squash.begin(), it));
            } else {
                squashMaskOffset = verify_u32(squash.size());
                squash.push_back(sit->second);
            }
        }

        // Add an accept (or acceptEod) per report ID.

        vector<NFAAccept> *accepts_out;
        if (edge(v, h.accept, h).second) {
            acceptMask.set(state_id);
            accepts_out = &accepts;
        } else {
            assert(edge(v, h.acceptEod, h).second);
            acceptEodMask.set(state_id);
            accepts_out = &acceptsEod;
        }

        for (auto report : h[v].reports) {
            accepts_out->push_back(NFAAccept());
            NFAAccept &a = accepts_out->back();
            a.state = state_id;
            a.externalId = report;
            a.squash = squashMaskOffset;
            DEBUG_PRINTF("Accept: state=%u, externalId=%u\n", state_id, report);
        }
    }
}

static
void buildTopMasks(const build_info &args, vector<NFAStateSet> &topMasks) {
    if (args.tops.empty()) {
        return; // No tops, probably an outfix NFA.
    }

    u32 numMasks = args.tops.rbegin()->first + 1; // max mask index
    DEBUG_PRINTF("we have %u top masks\n", numMasks);
    assert(numMasks <= NFA_MAX_TOP_MASKS);

    topMasks.assign(numMasks, NFAStateSet(args.num_states)); // all zeroes

    for (const auto &m : args.tops) {
        u32 mask_idx = m.first;
        u32 state_id = args.state_ids.at(m.second);
        DEBUG_PRINTF("state %u is in top mask %u\n", state_id, mask_idx);

        assert(mask_idx < numMasks);
        assert(state_id != NO_STATE);

        topMasks[mask_idx].set(state_id);
    }
}

static
u32 uncompressedStateSize(u32 num_states) {
    // Number of bytes required to store all our states.
    return ROUNDUP_N(num_states, 8)/8;
}

static
u32 compressedStateSize(const NGHolder &h, const NFAStateSet &maskedStates,
                        const ue2::unordered_map<NFAVertex, u32> &state_ids) {
    // Shrink state requirement to enough to fit the compressed largest reach.
    vector<u32> allreach(N_CHARS, 0);

    for (auto v : vertices_range(h)) {
        u32 i = state_ids.at(v);
        if (i == NO_STATE || maskedStates.test(i)) {
            continue;
        }
        const CharReach &cr = h[v].char_reach;
        for (size_t j = cr.find_first(); j != cr.npos; j = cr.find_next(j)) {
            allreach[j]++; // state 'i' can reach character 'j'.
        }
    }

    u32 maxreach = *max_element(allreach.begin(), allreach.end());
    DEBUG_PRINTF("max reach is %u\n", maxreach);
    return (maxreach + 7) / 8;
}

static
bool hasSquashableInitDs(const build_info &args) {
    const NGHolder &h = args.h;

    if (args.squashMap.empty()) {
        DEBUG_PRINTF("squash map is empty\n");
        return false;
    }

    NFAStateSet initDs(args.num_states);
    u32 sds_state = args.state_ids.at(h.startDs);
    if (sds_state == NO_STATE) {
        DEBUG_PRINTF("no states in initds\n");
        return false;
    }

    initDs.set(sds_state);

    /* TODO: simplify */

    // Check normal squash map.
    for (const auto &m : args.squashMap) {
        DEBUG_PRINTF("checking squash mask for state %u\n",
                     args.state_ids.at(m.first));
        NFAStateSet squashed = ~(m.second); // flip mask
        assert(squashed.size() == initDs.size());
        if (squashed.intersects(initDs)) {
            DEBUG_PRINTF("state %u squashes initds states\n",
                         args.state_ids.at(m.first));
            return true;
        }
    }

    // Check report squash map.
    for (const auto &m : args.reportSquashMap) {
        DEBUG_PRINTF("checking report squash mask for state %u\n",
                     args.state_ids.at(m.first));
        NFAStateSet squashed = ~(m.second); // flip mask
        assert(squashed.size() == initDs.size());
        if (squashed.intersects(initDs)) {
            DEBUG_PRINTF("state %u squashes initds states\n",
                         args.state_ids.at(m.first));
            return true;
        }
    }

    return false;
}

static
bool hasInitDsStates(const NGHolder &h,
                     const ue2::unordered_map<NFAVertex, u32> &state_ids) {
    if (state_ids.at(h.startDs) != NO_STATE) {
        return true;
    }

    if (is_triggered(h) && state_ids.at(h.start) != NO_STATE) {
        return true;
    }

    return false;
}

static
void findMaskedCompressionStates(const build_info &args,
                                 NFAStateSet &maskedStates) {
    const NGHolder &h = args.h;
    if (!generates_callbacks(h)) {
        // Rose leftfixes can mask out initds, which is worth doing if it will
        // stay on forever (i.e. it's not squashable).
        u32 sds_i = args.state_ids.at(h.startDs);
        if (sds_i != NO_STATE && !hasSquashableInitDs(args)) {
            maskedStates.set(sds_i);
            DEBUG_PRINTF("masking out initds state\n");
        }
    }

    // Suffixes and outfixes can mask out leaf states, which should all be
    // accepts. Right now we can only do this when there is nothing in initDs,
    // as we switch that on unconditionally in the expand call.
    if (!inspects_states_for_accepts(h)
        && !hasInitDsStates(h, args.state_ids)) {
        NFAStateSet nonleaf(args.num_states);
        for (const auto &e : edges_range(h)) {
            u32 from = args.state_ids.at(source(e, h));
            u32 to = args.state_ids.at(target(e, h));
            if (from == NO_STATE) {
                continue;
            }

            // We cannot mask out EOD accepts, as they have to perform an
            // action after they're switched on that may be delayed until the
            // next stream write.
            if (to == NO_STATE && target(e, h) != h.acceptEod) {
                continue;
            }

            nonleaf.set(from);
        }

        for (u32 i = 0; i < args.num_states; i++) {
            if (!nonleaf.test(i)) {
                maskedStates.set(i);
            }
        }

        DEBUG_PRINTF("masking out %zu leaf states\n", maskedStates.count());
    }
}

/** \brief Sets a given flag in the LimEx structure. */
template<class implNFA_t>
static
void setLimexFlag(implNFA_t *limex, u32 flag) {
    assert(flag);
    assert((flag & (flag - 1)) == 0);
    limex->flags |= flag;
}

/** \brief Sets a given flag in the NFA structure */
static
void setNfaFlag(NFA *nfa, u32 flag) {
    assert(flag);
    assert((flag & (flag - 1)) == 0);
    nfa->flags |= flag;
}

// Some of our NFA types support compressing the state down if we're not using
// all of it.
template<class implNFA_t>
static
void findStateSize(const build_info &args, implNFA_t *limex) {
    // Nothing is masked off by default.
    maskFill(limex->compressMask, 0xff);

    u32 sizeUncompressed = uncompressedStateSize(args.num_states);
    assert(sizeUncompressed <= sizeof(limex->compressMask));

    if (!args.stateCompression) {
        DEBUG_PRINTF("compression disabled, uncompressed state size %u\n",
                     sizeUncompressed);
        limex->stateSize = sizeUncompressed;
        return;
    }

    NFAStateSet maskedStates(args.num_states);
    findMaskedCompressionStates(args, maskedStates);

    u32 sizeCompressed = compressedStateSize(args.h, maskedStates, args.state_ids);
    assert(sizeCompressed <= sizeof(limex->compressMask));

    DEBUG_PRINTF("compressed=%u, uncompressed=%u\n", sizeCompressed,
                 sizeUncompressed);

    // Must be at least a 10% saving.
    if ((sizeCompressed * 100) <= (sizeUncompressed * 90)) {
        DEBUG_PRINTF("using compression, state size %u\n",
                     sizeCompressed);
        setLimexFlag(limex, LIMEX_FLAG_COMPRESS_STATE);
        limex->stateSize = sizeCompressed;

        if (maskedStates.any()) {
            DEBUG_PRINTF("masking %zu states\n", maskedStates.count());
            setLimexFlag(limex, LIMEX_FLAG_COMPRESS_MASKED);
            for (size_t i = maskedStates.find_first(); i != NFAStateSet::npos;
                    i = maskedStates.find_next(i)) {
                maskClearBit(limex->compressMask, i);
            }
        }
    } else {
        DEBUG_PRINTF("not using compression, state size %u\n",
                     sizeUncompressed);
        limex->stateSize = sizeUncompressed;
    }
}

/*
 * LimEx NFA: code for building NFAs in the Limited+Exceptional model. Most
 * transitions are limited, with transitions outside the constraints of our
 * shifts taken care of as 'exceptions'. Exceptions are also used to handle
 * accepts and squash behaviour.
 */

/**
 * \brief Prototype exception class.
 *
 * Used to build up the map of exceptions before being converted to real
 * NFAException32 (etc) structures.
 */
struct ExceptionProto {
    u32 reports_index = MO_INVALID_IDX;
    NFAStateSet succ_states;
    NFAStateSet squash_states;
    u32 repeat_index = MO_INVALID_IDX;
    enum LimExTrigger trigger = LIMEX_TRIGGER_NONE;
    enum LimExSquash squash = LIMEX_SQUASH_NONE;

    explicit ExceptionProto(u32 num_states)
        : succ_states(num_states), squash_states(num_states) {
        // Squash states are represented as the set of states to leave on,
        // so we start with all-ones.
        squash_states.set();
    }

    bool operator<(const ExceptionProto &b) const {
        const ExceptionProto &a = *this;

        ORDER_CHECK(reports_index);
        ORDER_CHECK(repeat_index);
        ORDER_CHECK(trigger);
        ORDER_CHECK(squash);
        ORDER_CHECK(succ_states);
        ORDER_CHECK(squash_states);

        return false;
    }
};

static
u32 getReportListIndex(const flat_set<ReportID> &reports,
                       vector<ReportID> &exceptionReports,
                       map<vector<ReportID>, u32> &reportListCache) {
    if (reports.empty()) {
        return MO_INVALID_IDX;
    }

    const vector<ReportID> r(reports.begin(), reports.end());

    auto it = reportListCache.find(r);
    if (it != reportListCache.end()) {
        u32 idx = it->second;
        assert(idx < exceptionReports.size());
        assert(equal(r.begin(), r.end(), exceptionReports.begin() + idx));
        return idx;
    }

    u32 idx = verify_u32(exceptionReports.size());
    reportListCache[r] = idx;
    exceptionReports.insert(exceptionReports.end(), r.begin(), r.end());
    exceptionReports.push_back(MO_INVALID_IDX); // terminator
    return idx;
}

static
void buildExceptionMap(const build_info &args,
                       const ue2::unordered_set<NFAEdge> &exceptional,
                       map<ExceptionProto, vector<u32> > &exceptionMap,
                       vector<ReportID> &exceptionReports) {
    const NGHolder &h = args.h;
    const u32 num_states = args.num_states;

    ue2::unordered_map<NFAVertex, u32> pos_trigger;
    ue2::unordered_map<NFAVertex, u32> tug_trigger;

    for (u32 i = 0; i < args.repeats.size(); i++) {
        const BoundedRepeatData &br = args.repeats[i];
        assert(!contains(pos_trigger, br.pos_trigger));
        pos_trigger[br.pos_trigger] = i;
        for (auto v : br.tug_triggers) {
            assert(!contains(tug_trigger, v));
            tug_trigger[v] = i;
        }
    }

    // We track report lists that have already been written into the global
    // list in case we can reuse them.
    map<vector<ReportID>, u32> reportListCache;

    for (auto v : vertices_range(h)) {
        const u32 i = args.state_ids.at(v);

        if (i == NO_STATE) {
            continue;
        }

        bool addMe = false;
        ExceptionProto e(num_states);

        if (edge(v, h.accept, h).second && generates_callbacks(h)) {
            /* if nfa is never used to produce callbacks, no need to mark
             * states as exceptional */
            const auto &reports = h[v].reports;

            DEBUG_PRINTF("state %u is exceptional due to accept "
                         "(%zu reports)\n", i, reports.size());

            e.reports_index =
                getReportListIndex(reports, exceptionReports, reportListCache);

            // We may be applying a report squash too.
            auto mi = args.reportSquashMap.find(v);
            if (mi != args.reportSquashMap.end()) {
                DEBUG_PRINTF("report squashes states\n");
                assert(e.squash_states.size() == mi->second.size());
                e.squash_states = mi->second;
                e.squash = LIMEX_SQUASH_REPORT;
            }

            addMe = true;
        }

        if (contains(pos_trigger, v)) {
            u32 repeat_index = pos_trigger[v];
            assert(e.trigger == LIMEX_TRIGGER_NONE);
            e.trigger = LIMEX_TRIGGER_POS;
            e.repeat_index = repeat_index;
            DEBUG_PRINTF("state %u has pos trigger for repeat %u\n", i,
                         repeat_index);
            addMe = true;
        }

        if (contains(tug_trigger, v)) {
            u32 repeat_index = tug_trigger[v];
            assert(e.trigger == LIMEX_TRIGGER_NONE);
            e.trigger = LIMEX_TRIGGER_TUG;
            e.repeat_index = repeat_index;

            // TUG triggers can squash the preceding cyclic state.
            u32 cyclic = args.state_ids.at(args.repeats[repeat_index].cyclic);
            e.squash_states.reset(cyclic);
            e.squash = LIMEX_SQUASH_TUG;
            DEBUG_PRINTF("state %u has tug trigger for repeat %u, can squash "
                         "state %u\n", i, repeat_index, cyclic);
            addMe = true;
        }

        // are we a non-limited transition?
        for (const auto &oe : out_edges_range(v, h)) {
            if (contains(exceptional, oe)) {
                NFAVertex w = target(oe, h);
                u32 w_idx = args.state_ids.at(w);
                assert(w_idx != NO_STATE);
                e.succ_states.set(w_idx);
                DEBUG_PRINTF("exceptional transition %u->%u\n", i, w_idx);
                addMe = true;
            }
        }

        // do we lead SOLELY to a squasher state? (we use the successors as
        // a proxy for the out-edge here, so there must be only one for us
        // to do this safely)
        /* The above comment is IMHO bogus and would result in all squashing
         * being disabled around stars */
        if (e.trigger != LIMEX_TRIGGER_TUG) {
            for (auto w : adjacent_vertices_range(v, h)) {
                if (w == v) {
                    continue;
                }
                u32 j = args.state_ids.at(w);
                if (j == NO_STATE) {
                    continue;
                }
                DEBUG_PRINTF("we are checking if succ %u is a squasher\n", j);
                auto mi = args.squashMap.find(w);
                if (mi != args.squashMap.end()) {
                    DEBUG_PRINTF("squasher edge (%u, %u)\n", i, j);
                    DEBUG_PRINTF("e.squash_states.size() == %zu, "
                                 "mi->second.size() = %zu\n",
                                 e.squash_states.size(), mi->second.size());
                    assert(e.squash_states.size() == mi->second.size());
                    e.squash_states = mi->second;

                    // NOTE: this might be being combined with the report
                    // squashing above.

                    e.squash = LIMEX_SQUASH_CYCLIC;
                    DEBUG_PRINTF("squashing succ %u (turns off %zu states)\n",
                                 j, mi->second.size() - mi->second.count());
                    addMe = true;
                }
            }
        }

        if (addMe) {
            // Add 'e' if it isn't in the map, and push state i on to its list
            // of states.
            assert(e.succ_states.size() == num_states);
            assert(e.squash_states.size() == num_states);
            exceptionMap[e].push_back(i);
        }
    }

    DEBUG_PRINTF("%zu unique exceptions found.\n", exceptionMap.size());
}

static
u32 depth_to_u32(const depth &d) {
    assert(d.is_reachable());
    if (d.is_infinite()) {
        return REPEAT_INF;
    }

    u32 d_val = d;
    assert(d_val < REPEAT_INF);
    return d_val;
}

static
bool isExceptionalTransition(const NGHolder &h, const NFAEdge &e,
                             const build_info &args, u32 maxShift) {
    NFAVertex from = source(e, h);
    NFAVertex to = target(e, h);
    u32 f = args.state_ids.at(from);
    u32 t = args.state_ids.at(to);
    if (!isLimitedTransition(f, t, maxShift)) {
        return true;
    }

    // All transitions out of a tug trigger are exceptional.
    if (contains(args.tugs, from)) {
        return true;
    }
    return false;
}

static
u32 findMaxVarShift(const build_info &args, u32 nShifts) {
    const NGHolder &h = args.h;
    u32 shiftMask = 0;
    for (const auto &e : edges_range(h)) {
        u32 from = args.state_ids.at(source(e, h));
        u32 to = args.state_ids.at(target(e, h));
        if (from == NO_STATE || to == NO_STATE) {
            continue;
        }
        if (!isExceptionalTransition(h, e, args, MAX_SHIFT_AMOUNT)) {
            shiftMask |= (1UL << (to - from));
        }
    }

    u32 maxVarShift = 0;
    for (u32 shiftCnt = 0; shiftMask != 0 && shiftCnt < nShifts; shiftCnt++) {
        maxVarShift = findAndClearLSB_32(&shiftMask);
    }

    return maxVarShift;
}

static
int getLimexScore(const build_info &args, u32 nShifts) {
    const NGHolder &h = args.h;
    u32 maxVarShift = nShifts;
    int score = 0;

    score += SHIFT_COST * nShifts;
    maxVarShift = findMaxVarShift(args, nShifts);

    NFAStateSet exceptionalStates(args.num_states);
    for (const auto &e : edges_range(h)) {
        u32 from = args.state_ids.at(source(e, h));
        u32 to = args.state_ids.at(target(e, h));
        if (from == NO_STATE || to == NO_STATE) {
            continue;
        }
        if (isExceptionalTransition(h, e, args, maxVarShift)) {
            exceptionalStates.set(from);
        }
    }
    score += EXCEPTION_COST * exceptionalStates.count();
    return score;
}

// This function finds the best shift scheme with highest score
// Returns number of shifts and score calculated for appropriate scheme
// Returns zero if no appropriate scheme was found
static
u32 findBestNumOfVarShifts(const build_info &args,
                           int *bestScoreRet = nullptr) {
    u32 bestNumOfVarShifts = 0;
    int bestScore = INT_MAX;
    for (u32 shiftCount = 1; shiftCount <= MAX_SHIFT_COUNT; shiftCount++) {
        int score = getLimexScore(args, shiftCount);
        if (score < bestScore) {
            bestScore = score;
            bestNumOfVarShifts = shiftCount;
        }
    }
    if (bestScoreRet != nullptr) {
        *bestScoreRet = bestScore;
    }
    return bestNumOfVarShifts;
}

template<NFAEngineType dtype>
struct Factory {
    // typedefs for readability, for types derived from traits
    typedef typename NFATraits<dtype>::exception_t exception_t;
    typedef typename NFATraits<dtype>::implNFA_t implNFA_t;
    typedef typename NFATraits<dtype>::tableRow_t tableRow_t;

    static
    void allocState(NFA *nfa, u32 repeatscratchStateSize,
                    u32 repeatStreamState) {
        implNFA_t *limex = (implNFA_t *)getMutableImplNfa(nfa);

        // LimEx NFAs now store the following in state:
        // 1. state bitvector (always present)
        // 2. space associated with repeats
        // This function just needs to size these correctly.

        u32 stateSize = limex->stateSize;

        DEBUG_PRINTF("bitvector=%zu/%u, repeat full=%u, stream=%u\n",
                     sizeof(limex->init), stateSize, repeatscratchStateSize,
                     repeatStreamState);

        size_t scratchStateSize = sizeof(limex->init);
        if (repeatscratchStateSize) {
            scratchStateSize
                = ROUNDUP_N(scratchStateSize, alignof(RepeatControl));
            scratchStateSize += repeatscratchStateSize;
        }
        size_t streamStateSize = stateSize + repeatStreamState;

        nfa->scratchStateSize = verify_u32(scratchStateSize);
        nfa->streamStateSize = verify_u32(streamStateSize);
    }

    static
    size_t repeatAllocSize(const BoundedRepeatData &br, u32 *tableOffset,
                           u32 *tugMaskOffset) {
        size_t len = sizeof(NFARepeatInfo) + sizeof(RepeatInfo);

        // sparse lookup table.
        if (br.type == REPEAT_SPARSE_OPTIMAL_P) {
            len = ROUNDUP_N(len, alignof(u64a));
            *tableOffset = verify_u32(len);
            len += sizeof(u64a) * (br.repeatMax + 1);
        } else {
            *tableOffset = 0;
        }

        // tug mask.
        len = ROUNDUP_N(len, alignof(tableRow_t));
        *tugMaskOffset = verify_u32(len);
        len += sizeof(tableRow_t);

        // to simplify layout.
        len = ROUNDUP_CL(len);

        return len;
    }

    static
    void buildRepeats(const build_info &args,
                vector<pair<aligned_unique_ptr<NFARepeatInfo>, size_t>> &out,
                u32 *scratchStateSize, u32 *streamState) {
        out.reserve(args.repeats.size());

        u32 repeat_idx = 0;
        for (auto it = args.repeats.begin(), ite = args.repeats.end();
             it != ite; ++it, ++repeat_idx) {
            const BoundedRepeatData &br = *it;
            assert(args.state_ids.at(br.cyclic) != NO_STATE);

            u32 tableOffset, tugMaskOffset;
            size_t len = repeatAllocSize(br, &tableOffset, &tugMaskOffset);
            auto info = aligned_zmalloc_unique<NFARepeatInfo>(len);
            char *info_ptr = (char *)info.get();

            // Collect state space info.
            RepeatStateInfo rsi(br.type, br.repeatMin, br.repeatMax, br.minPeriod);
            u32 streamStateLen = rsi.packedCtrlSize + rsi.stateSize;

            // Fill the NFARepeatInfo structure.
            info->cyclicState = args.state_ids.at(br.cyclic);
            info->ctrlIndex = repeat_idx;
            info->packedCtrlOffset = *streamState;
            info->stateOffset = *streamState + rsi.packedCtrlSize;
            info->stateSize = streamStateLen;
            info->tugMaskOffset = tugMaskOffset;

            // Fill the RepeatInfo structure.
            RepeatInfo *repeat =
                (RepeatInfo *)(info_ptr + sizeof(NFARepeatInfo));
            repeat->type = br.type;
            repeat->repeatMin = depth_to_u32(br.repeatMin);
            repeat->repeatMax = depth_to_u32(br.repeatMax);
            repeat->horizon = rsi.horizon;
            repeat->packedCtrlSize = rsi.packedCtrlSize;
            repeat->stateSize = rsi.stateSize;
            copy_bytes(repeat->packedFieldSizes, rsi.packedFieldSizes);
            repeat->patchCount = rsi.patchCount;
            repeat->patchSize = rsi.patchSize;
            repeat->encodingSize = rsi.encodingSize;
            repeat->patchesOffset = rsi.patchesOffset;

            u32 repeat_len = sizeof(RepeatInfo);
            if (br.type == REPEAT_SPARSE_OPTIMAL_P) {
                repeat_len += sizeof(u64a) * (rsi.patchSize + 1);
            }
            repeat->length = repeat_len;

            // Copy in the sparse lookup table.
            if (br.type == REPEAT_SPARSE_OPTIMAL_P) {
                assert(!rsi.table.empty());
                copy_bytes(info_ptr + tableOffset, rsi.table);
            }

            // Fill the tug mask.
            tableRow_t *tugMask = (tableRow_t *)(info_ptr + tugMaskOffset);
            for (auto v : br.tug_triggers) {
                u32 state_id = args.state_ids.at(v);
                assert(state_id != NO_STATE);
                maskSetBit(*tugMask, state_id);
            }

            assert(streamStateLen);
            *streamState += streamStateLen;
            *scratchStateSize += sizeof(RepeatControl);

            out.emplace_back(move(info), len);
        }
    }

    static
    void writeLimexMasks(const build_info &args, implNFA_t *limex) {
        const NGHolder &h = args.h;

        // Init masks.
        u32 s_i = args.state_ids.at(h.start);
        u32 sds_i = args.state_ids.at(h.startDs);

        if (s_i != NO_STATE) {
            maskSetBit(limex->init, s_i);
            if (is_triggered(h)) {
                maskSetBit(limex->initDS, s_i);
            }
        }

        if (sds_i != NO_STATE) {
            maskSetBit(limex->init, sds_i);
            maskSetBit(limex->initDS, sds_i);
        }

        // Zombie mask.
        for (auto v : args.zombies) {
            u32 state_id = args.state_ids.at(v);
            assert(state_id != NO_STATE);
            maskSetBit(limex->zombieMask, state_id);
        }

        // Repeat cyclic mask.
        for (const auto &br : args.repeats) {
            u32 cyclic = args.state_ids.at(br.cyclic);
            assert(cyclic != NO_STATE);
            maskSetBit(limex->repeatCyclicMask, cyclic);
        }
    }

    static
    void writeShiftMasks(const build_info &args, implNFA_t *limex) {
        const NGHolder &h = args.h;
        u32 maxShift = findMaxVarShift(args, limex->shiftCount);
        u32 shiftMask = 0;
        int shiftMaskIdx = 0;

        for (const auto &e : edges_range(h)) {
            u32 from = args.state_ids.at(source(e, h));
            u32 to = args.state_ids.at(target(e, h));
            if (from == NO_STATE || to == NO_STATE) {
                continue;
            }

            // We check for exceptional transitions here, as we don't want tug
            // trigger transitions emitted as limited transitions (even if they
            // could be in this model).
            if (!isExceptionalTransition(h, e, args, maxShift)) {
                u32 shift = to - from;
                if ((shiftMask & (1UL << shift)) == 0UL) {
                    shiftMask |= (1UL << shift);
                    limex->shiftAmount[shiftMaskIdx++] = (u8)shift;
                }
                assert(limex->shiftCount <= MAX_SHIFT_COUNT);
                for (u32 i = 0; i < limex->shiftCount; i++) {
                    if (limex->shiftAmount[i] == (u8)shift) {
                        maskSetBit(limex->shift[i], from);
                        break;
                    }
                }
            }
        }
        if (maxShift && limex->shiftCount > 1) {
            for (u32 i = 0; i < limex->shiftCount; i++) {
                assert(!isMaskZero(limex->shift[i]));
            }
        }
    }

    static
    void findExceptionalTransitions(const build_info &args,
                                    ue2::unordered_set<NFAEdge> &exceptional,
                                    u32 maxShift) {
        const NGHolder &h = args.h;

        for (const auto &e : edges_range(h)) {
            u32 from = args.state_ids.at(source(e, h));
            u32 to = args.state_ids.at(target(e, h));
            if (from == NO_STATE || to == NO_STATE) {
                continue;
            }

            if (isExceptionalTransition(h, e, args, maxShift)) {
                exceptional.insert(e);
            }
        }
    }

    static
    void writeExceptions(const map<ExceptionProto, vector<u32> > &exceptionMap,
                         const vector<u32> &repeatOffsets,
                         implNFA_t *limex, const u32 exceptionsOffset) {
        DEBUG_PRINTF("exceptionsOffset=%u\n", exceptionsOffset);

        // to make testing easier, we pre-set the exceptionMap to all invalid
        // values
        memset(limex->exceptionMap, 0xff, sizeof(limex->exceptionMap));

        exception_t *etable = (exception_t *)((char *)limex + exceptionsOffset);
        assert(ISALIGNED(etable));

        u32 ecount = 0;
        for (const auto &m : exceptionMap) {
            const ExceptionProto &proto = m.first;
            const vector<u32> &states = m.second;
            DEBUG_PRINTF("exception %u, triggered by %zu states.\n", ecount,
                         states.size());

            // Write the exception entry.
            exception_t &e = etable[ecount];
            maskSetBits(e.squash, proto.squash_states);
            maskSetBits(e.successors, proto.succ_states);
            e.reports = proto.reports_index;
            e.hasSquash = verify_u8(proto.squash);
            e.trigger = verify_u8(proto.trigger);
            u32 repeat_offset = proto.repeat_index == MO_INVALID_IDX
                                    ? MO_INVALID_IDX
                                    : repeatOffsets[proto.repeat_index];
            e.repeatOffset = repeat_offset;

            // for each state that can switch it on
            for (auto state_id : states) {
                // set this bit in the exception mask
                maskSetBit(limex->exceptionMask, state_id);
                // set this index in the exception map
                limex->exceptionMap[state_id] = ecount;
            }
            ecount++;
        }

        limex->exceptionOffset = exceptionsOffset;
        limex->exceptionCount = ecount;
    }

    static
    void writeReachMapping(const vector<NFAStateSet> &reach,
                           const vector<u8> &reachMap, implNFA_t *limex,
                           const u32 reachOffset) {
        DEBUG_PRINTF("reachOffset=%u\n", reachOffset);

        // Reach mapping is inside the LimEx structure.
        copy(reachMap.begin(), reachMap.end(), &limex->reachMap[0]);

        // Reach table is right after the LimEx structure.
        tableRow_t *reachMask = (tableRow_t *)((char *)limex + reachOffset);
        assert(ISALIGNED(reachMask));
        for (size_t i = 0, end = reach.size(); i < end; i++) {
            maskSetBits(reachMask[i], reach[i]);
        }
        limex->reachSize = verify_u32(reach.size());
    }

    static
    void writeTopMasks(const vector<NFAStateSet> &tops, implNFA_t *limex,
                       const u32 topsOffset) {
        DEBUG_PRINTF("topsOffset=%u\n", topsOffset);

        limex->topOffset = topsOffset;
        tableRow_t *topMasks = (tableRow_t *)((char *)limex + topsOffset);
        assert(ISALIGNED(topMasks));

        for (size_t i = 0, end = tops.size(); i < end; i++) {
            maskSetBits(topMasks[i], tops[i]);
        }

        limex->topCount = verify_u32(tops.size());
    }

    static
    void writeAccelSsse3Masks(const NFAStateSet &accelMask, implNFA_t *limex) {
        char *perm_base = (char *)&limex->accelPermute;
        char *comp_base = (char *)&limex->accelCompare;

        u32 num = 0; // index in accel table.
        for (size_t i = accelMask.find_first(); i != accelMask.npos;
             i = accelMask.find_next(i), ++num) {
            u32 state_id = verify_u32(i);
            DEBUG_PRINTF("accel num=%u, state=%u\n", num, state_id);

            // PSHUFB permute and compare masks
            size_t mask_idx = sizeof(u_128) * (state_id / 128U);
            DEBUG_PRINTF("mask_idx=%zu\n", mask_idx);
            u_128 *perm = (u_128 *)(perm_base + mask_idx);
            u_128 *comp = (u_128 *)(comp_base + mask_idx);
            maskSetByte(*perm, num, ((state_id % 128U) / 8U));
            maskSetByte(*comp, num, ~(1U << (state_id % 8U)));
        }
    }

    static
    void writeAccel(const NFAStateSet &accelMask,
                    const NFAStateSet &accelFriendsMask,
                    const AccelAuxVector &accelAux,
                    const vector<u8> &accelTable, implNFA_t *limex,
                    const u32 accelTableOffset, const u32 accelAuxOffset) {
        DEBUG_PRINTF("accelTableOffset=%u, accelAuxOffset=%u\n",
                      accelTableOffset, accelAuxOffset);

        // Write accel lookup table.
        limex->accelTableOffset = accelTableOffset;
        copy(accelTable.begin(), accelTable.end(),
             (u8 *)((char *)limex + accelTableOffset));

        // Write accel aux structures.
        limex->accelAuxOffset = accelAuxOffset;
        AccelAux *auxTable = (AccelAux *)((char *)limex + accelAuxOffset);
        assert(ISALIGNED(auxTable));
        copy(accelAux.begin(), accelAux.end(), auxTable);

        // Write LimEx structure members.
        limex->accelCount = verify_u32(accelTable.size());
        // FIXME: accelAuxCount is unused?
        limex->accelAuxCount = verify_u32(accelAux.size());

        // Write LimEx masks.
        maskSetBits(limex->accel, accelMask);
        maskSetBits(limex->accel_and_friends, accelFriendsMask);

        // We can use PSHUFB-based shuffles for models >= 128 states. These
        // require some additional masks in the bytecode.
        maskClear(limex->accelCompare);
        maskFill(limex->accelPermute, (char)0x80);
        if (NFATraits<dtype>::maxStates >= 128) {
            writeAccelSsse3Masks(accelMask, limex);
        }
    }

    static
    void writeAccepts(const NFAStateSet &acceptMask,
                      const NFAStateSet &acceptEodMask,
                      const vector<NFAAccept> &accepts,
                      const vector<NFAAccept> &acceptsEod,
                      const vector<NFAStateSet> &squash, implNFA_t *limex,
                      const u32 acceptsOffset, const u32 acceptsEodOffset,
                      const u32 squashOffset) {
        DEBUG_PRINTF("acceptsOffset=%u, acceptsEodOffset=%u, squashOffset=%u\n",
                     acceptsOffset, acceptsEodOffset, squashOffset);

        // LimEx masks (in structure)
        maskSetBits(limex->accept, acceptMask);
        maskSetBits(limex->acceptAtEOD, acceptEodMask);

        // Write accept table.
        limex->acceptOffset = acceptsOffset;
        limex->acceptCount = verify_u32(accepts.size());
        DEBUG_PRINTF("NFA has %zu accepts\n", accepts.size());
        NFAAccept *acceptsTable = (NFAAccept *)((char *)limex + acceptsOffset);
        assert(ISALIGNED(acceptsTable));
        copy(accepts.begin(), accepts.end(), acceptsTable);

        // Write eod accept table.
        limex->acceptEodOffset = acceptsEodOffset;
        limex->acceptEodCount = verify_u32(acceptsEod.size());
        DEBUG_PRINTF("NFA has %zu EOD accepts\n", acceptsEod.size());
        NFAAccept *acceptsEodTable = (NFAAccept *)((char *)limex + acceptsEodOffset);
        assert(ISALIGNED(acceptsEodTable));
        copy(acceptsEod.begin(), acceptsEod.end(), acceptsEodTable);

        // Write squash mask table.
        limex->squashCount = verify_u32(squash.size());
        limex->squashOffset = squashOffset;
        DEBUG_PRINTF("NFA has %zu report squash masks\n", squash.size());
        tableRow_t *mask = (tableRow_t *)((char *)limex + squashOffset);
        assert(ISALIGNED(mask));
        for (size_t i = 0, end = squash.size(); i < end; i++) {
            maskSetBits(mask[i], squash[i]);
        }
    }

    static
    void writeRepeats(const vector<pair<aligned_unique_ptr<NFARepeatInfo>,
                                        size_t>> &repeats,
                      vector<u32> &repeatOffsets, implNFA_t *limex,
                      const u32 repeatOffsetsOffset, const u32 repeatOffset) {
        const u32 num_repeats = verify_u32(repeats.size());

        DEBUG_PRINTF("repeatOffsetsOffset=%u, repeatOffset=%u\n",
                      repeatOffsetsOffset, repeatOffset);

        repeatOffsets.resize(num_repeats);
        u32 offset = repeatOffset;

        for (u32 i = 0; i < num_repeats; i++) {
            repeatOffsets[i] = offset;
            assert(repeats[i].first);
            memcpy((char *)limex + offset, repeats[i].first.get(),
                   repeats[i].second);
            offset += repeats[i].second;
        }

        // Write repeat offset lookup table.
        assert(ISALIGNED_N((char *)limex + repeatOffsetsOffset, alignof(u32)));
        copy_bytes((char *)limex + repeatOffsetsOffset, repeatOffsets);

        limex->repeatOffset = repeatOffsetsOffset;
        limex->repeatCount = num_repeats;
    }

    static
    void writeExceptionReports(const vector<ReportID> &reports,
                               implNFA_t *limex,
                               const u32 exceptionReportsOffset) {
        DEBUG_PRINTF("exceptionReportsOffset=%u\n", exceptionReportsOffset);

        limex->exReportOffset = exceptionReportsOffset;
        assert(ISALIGNED_N((char *)limex + exceptionReportsOffset,
                           alignof(ReportID)));
        copy_bytes((char *)limex + exceptionReportsOffset, reports);
    }

    static
    aligned_unique_ptr<NFA> generateNfa(const build_info &args) {
        if (args.num_states > NFATraits<dtype>::maxStates) {
            return nullptr;
        }

        // Build bounded repeat structures.
        vector<pair<aligned_unique_ptr<NFARepeatInfo>, size_t>> repeats;
        u32 repeats_full_state = 0;
        u32 repeats_stream_state = 0;
        buildRepeats(args, repeats, &repeats_full_state, &repeats_stream_state);
        size_t repeatSize = 0;
        for (size_t i = 0; i < repeats.size(); i++) {
            repeatSize += repeats[i].second;
        }

        ue2::unordered_set<NFAEdge> exceptional;
        u32 shiftCount = findBestNumOfVarShifts(args);
        assert(shiftCount);
        u32 maxShift = findMaxVarShift(args, shiftCount);
        findExceptionalTransitions(args, exceptional, maxShift);

        map<ExceptionProto, vector<u32> > exceptionMap;
        vector<ReportID> exceptionReports;
        buildExceptionMap(args, exceptional, exceptionMap, exceptionReports);

        if (exceptionMap.size() > ~0U) {
            DEBUG_PRINTF("too many exceptions!\n");
            return nullptr;
        }

        // Build reach table and character mapping.
        vector<NFAStateSet> reach;
        vector<u8> reachMap;
        buildReachMapping(args, reach, reachMap);

        // Build top masks.
        vector<NFAStateSet> tops;
        buildTopMasks(args, tops);

        // Build all our accept info.
        NFAStateSet acceptMask, acceptEodMask;
        vector<NFAAccept> accepts, acceptsEod;
        vector<NFAStateSet> squash;
        buildAccepts(args, acceptMask, acceptEodMask, accepts, acceptsEod,
                     squash);

        // Build all our accel info.
        NFAStateSet accelMask, accelFriendsMask;
        AccelAuxVector accelAux;
        vector<u8> accelTable;
        buildAccel(args, accelMask, accelFriendsMask, accelAux, accelTable);

        // Compute the offsets in the bytecode for this LimEx NFA for all of
        // our structures. First, the NFA and LimEx structures. All other
        // offsets are relative to the start of the LimEx struct, starting with
        // the reach table.
        u32 offset = sizeof(implNFA_t);

        const u32 reachOffset = offset;
        offset += sizeof(tableRow_t) * reach.size();

        const u32 topsOffset = offset;
        offset += sizeof(tableRow_t) * tops.size();

        const u32 accelTableOffset = offset;
        offset += sizeof(u8) * accelTable.size();

        offset = ROUNDUP_N(offset, alignof(AccelAux));
        const u32 accelAuxOffset = offset;
        offset += sizeof(AccelAux) * accelAux.size();

        offset = ROUNDUP_N(offset, alignof(NFAAccept));
        const u32 acceptsOffset = offset;
        offset += sizeof(NFAAccept) * accepts.size();
        const u32 acceptsEodOffset = offset;
        offset += sizeof(NFAAccept) * acceptsEod.size();

        offset = ROUNDUP_CL(offset);
        const u32 squashOffset = offset;
        offset += sizeof(tableRow_t) * squash.size();

        offset = ROUNDUP_CL(offset);
        const u32 exceptionsOffset = offset;
        offset += sizeof(exception_t) * exceptionMap.size();

        const u32 exceptionReportsOffset = offset;
        offset += sizeof(ReportID) * exceptionReports.size();

        const u32 repeatOffsetsOffset = offset;
        offset += sizeof(u32) * args.repeats.size();

        offset = ROUNDUP_CL(offset);
        const u32 repeatsOffset = offset;
        offset += repeatSize;

        // Now we can allocate space for the NFA and get to work on layout.

        size_t nfaSize = sizeof(NFA) + offset;
        DEBUG_PRINTF("nfa size %zu\n", nfaSize);
        auto nfa = aligned_zmalloc_unique<NFA>(nfaSize);
        assert(nfa); // otherwise we would have thrown std::bad_alloc

        implNFA_t *limex = (implNFA_t *)getMutableImplNfa(nfa.get());
        assert(ISALIGNED(limex));

        writeReachMapping(reach, reachMap, limex, reachOffset);

        writeTopMasks(tops, limex, topsOffset);

        writeAccel(accelMask, accelFriendsMask, accelAux, accelTable,
                   limex, accelTableOffset, accelAuxOffset);

        writeAccepts(acceptMask, acceptEodMask, accepts, acceptsEod, squash,
                     limex, acceptsOffset, acceptsEodOffset, squashOffset);

        limex->shiftCount = shiftCount;
        writeShiftMasks(args, limex);

        // Determine the state required for our state vector.
        findStateSize(args, limex);

        writeExceptionReports(exceptionReports, limex, exceptionReportsOffset);

        // Repeat structures and offset table.
        vector<u32> repeatOffsets;
        writeRepeats(repeats, repeatOffsets, limex, repeatOffsetsOffset,
                     repeatsOffset);

        writeExceptions(exceptionMap, repeatOffsets, limex, exceptionsOffset);

        writeLimexMasks(args, limex);

        allocState(nfa.get(), repeats_full_state, repeats_stream_state);

        nfa->type = dtype;
        nfa->length = verify_u32(nfaSize);
        nfa->nPositions = args.num_states;

        if (!args.zombies.empty()) {
            setNfaFlag(nfa.get(), NFA_ZOMBIE);
        }
        if (!acceptsEod.empty()) {
            setNfaFlag(nfa.get(), NFA_ACCEPTS_EOD);
        }

        return nfa;
    }

    static int score(const build_info &args) {
        // LimEx NFAs are available in sizes from 32 to 512-bit.
        size_t num_states = args.num_states;

        size_t sz = findContainerSize(num_states);
        if (sz < 32) {
            sz = 32;
        }

        // Special case: with SIMD available, we definitely prefer using
        // 128-bit NFAs over 64-bit ones given the paucity of registers
        // available.
        if (sz == 64) {
            sz = 128;
        }

        if (args.cc.grey.nfaForceSize) {
            sz = args.cc.grey.nfaForceSize;
        }

        if (sz != NFATraits<dtype>::maxStates) {
            return -1; // fail, size not appropriate
        }

        // We are of the right size, calculate a score based on the number
        // of exceptions and the number of shifts used by this LimEx.
        int score;
        u32 shiftCount = findBestNumOfVarShifts(args, &score);
        if (shiftCount == 0) {
            return -1;
        }
        return score;
    }
};

template<NFAEngineType dtype>
struct generateNfa {
    static aligned_unique_ptr<NFA> call(const build_info &args) {
        return Factory<dtype>::generateNfa(args);
    }
};

template<NFAEngineType dtype>
struct scoreNfa {
    static int call(const build_info &args) {
        return Factory<dtype>::score(args);
    }
};

#define MAKE_LIMEX_TRAITS(mlt_size)                                            \
    template<> struct NFATraits<LIMEX_NFA_##mlt_size> {                        \
        typedef LimExNFA##mlt_size implNFA_t;                                  \
        typedef u_##mlt_size tableRow_t;                                       \
        typedef NFAException##mlt_size exception_t;                            \
        static const size_t maxStates = mlt_size;                              \
    };

MAKE_LIMEX_TRAITS(32)
MAKE_LIMEX_TRAITS(128)
MAKE_LIMEX_TRAITS(256)
MAKE_LIMEX_TRAITS(384)
MAKE_LIMEX_TRAITS(512)

} // namespace

#ifndef NDEBUG
// Some sanity tests, called by an assertion in generate().
static UNUSED
bool isSane(const NGHolder &h, const map<u32, NFAVertex> &tops,
            const ue2::unordered_map<NFAVertex, u32> &state_ids,
            u32 num_states) {
    ue2::unordered_set<u32> seen;
    ue2::unordered_set<NFAVertex> top_starts;
    for (const auto &m : tops) {
        top_starts.insert(m.second);
    }

    for (auto v : vertices_range(h)) {
        if (!contains(state_ids, v)) {
            DEBUG_PRINTF("no entry for vertex %u in state map\n",
                         h[v].index);
            return false;
        }
        const u32 i = state_ids.at(v);
        if (i == NO_STATE) {
            continue;
        }

        DEBUG_PRINTF("checking vertex %u (state %u)\n", h[v].index,
                     i);

        if (i >= num_states || contains(seen, i)) {
            DEBUG_PRINTF("vertex %u/%u has invalid state\n", i, num_states);
            return false;
        }
        seen.insert(i);

        // All our states should be reachable and have a state assigned.
        if (h[v].char_reach.none()) {
            DEBUG_PRINTF("vertex %u has empty reachability\n", h[v].index);
            return false;
        }

        // Every state that isn't a start state (or top, in triggered NFAs)
        // must have at least one predecessor that is not itself.
        if (v != h.start && v != h.startDs && !contains(top_starts, v)
            && !proper_in_degree(v, h)) {
            DEBUG_PRINTF("vertex %u has no pred\n", h[v].index);
            return false;
        }
    }

    if (seen.size() != num_states) {
        return false;
    }

    return true;
}
#endif // NDEBUG

static
u32 max_state(const ue2::unordered_map<NFAVertex, u32> &state_ids) {
    u32 rv = 0;
    for (const auto &m : state_ids) {
        DEBUG_PRINTF("state %u\n", m.second);
        if (m.second != NO_STATE) {
            rv = max(m.second, rv);
        }
    }
    DEBUG_PRINTF("max %u\n", rv);
    return rv;
}

aligned_unique_ptr<NFA> generate(NGHolder &h,
                         const ue2::unordered_map<NFAVertex, u32> &states,
                         const vector<BoundedRepeatData> &repeats,
                         const map<NFAVertex, NFAStateSet> &reportSquashMap,
                         const map<NFAVertex, NFAStateSet> &squashMap,
                         const map<u32, NFAVertex> &tops,
                         const set<NFAVertex> &zombies,
                         bool do_accel,
                         bool stateCompression,
                         u32 hint,
                         const CompileContext &cc) {
    const u32 num_states = max_state(states) + 1;
    DEBUG_PRINTF("total states: %u\n", num_states);

    if (!cc.grey.allowLimExNFA) {
        DEBUG_PRINTF("limex not allowed\n");
        return nullptr;
    }

    // If you ask for a particular type, it had better be an NFA.
    assert(hint == INVALID_NFA || hint <= LAST_LIMEX_NFA);
    DEBUG_PRINTF("hint=%u\n", hint);

    // Sanity check the input data.
    assert(isSane(h, tops, states, num_states));

    // Build arguments used in the rest of this file.
    build_info arg(h, states, repeats, reportSquashMap, squashMap, tops,
                   zombies, do_accel, stateCompression, cc, num_states);

    // Acceleration analysis.
    fillAccelInfo(arg);

    vector<pair<int, NFAEngineType>> scores;

    if (hint != INVALID_NFA) {
        // The caller has told us what to (attempt to) build.
        scores.emplace_back(0, (NFAEngineType)hint);
    } else {
        for (size_t i = 0; i <= LAST_LIMEX_NFA; i++) {
            NFAEngineType ntype = (NFAEngineType)i;
            int score = DISPATCH_BY_LIMEX_TYPE(ntype, scoreNfa, arg);
            if (score >= 0) {
                DEBUG_PRINTF("%s scores %d\n", nfa_type_name(ntype), score);
                scores.emplace_back(score, ntype);
            }
        }
    }

    if (scores.empty()) {
        DEBUG_PRINTF("No NFA returned a valid score for this case.\n");
        return nullptr;
    }

    // Sort acceptable models in priority order, lowest score first.
    sort(scores.begin(), scores.end());

    for (const auto &elem : scores) {
        assert(elem.first >= 0);
        NFAEngineType limex_model = elem.second;
        auto nfa = DISPATCH_BY_LIMEX_TYPE(limex_model, generateNfa, arg);
        if (nfa) {
            DEBUG_PRINTF("successful build with NFA engine: %s\n",
                         nfa_type_name(limex_model));
            return nfa;
        }
    }

    DEBUG_PRINTF("NFA build failed.\n");
    return nullptr;
}

u32 countAccelStates(NGHolder &h,
                     const ue2::unordered_map<NFAVertex, u32> &states,
                     const vector<BoundedRepeatData> &repeats,
                     const map<NFAVertex, NFAStateSet> &reportSquashMap,
                     const map<NFAVertex, NFAStateSet> &squashMap,
                     const map<u32, NFAVertex> &tops,
                     const set<NFAVertex> &zombies,
                     const CompileContext &cc) {
    const u32 num_states = max_state(states) + 1;
    DEBUG_PRINTF("total states: %u\n", num_states);

    if (!cc.grey.allowLimExNFA) {
        DEBUG_PRINTF("limex not allowed\n");
        return 0;
    }

    // Sanity check the input data.
    assert(isSane(h, tops, states, num_states));

    const bool do_accel = true;
    const bool state_compression = false;

    // Build arguments used in the rest of this file.
    build_info bi(h, states, repeats, reportSquashMap, squashMap, tops, zombies,
                  do_accel, state_compression, cc, num_states);

    // Acceleration analysis.
    nfaFindAccelSchemes(bi.h, bi.br_cyclic, &bi.accel.accel_map);

    u32 num_accel = verify_u32(bi.accel.accel_map.size());
    DEBUG_PRINTF("found %u accel states\n", num_accel);
    return num_accel;
}

} // namespace ue2

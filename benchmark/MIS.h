// This code is part of the project "Theoretically Efficient Parallel Graph
// Algorithms Can Be Fast and Scalable", presented at Symposium on Parallelism
// in Algorithms and Architectures, 2018.
// Copyright (c) 2018 Laxman Dhulipala, Guy Blelloch, and Julian Shun
//
// Permission is hereby granted, free of charge, to any person obtaining a copy
// of this software and associated documentation files (the "Software"), to deal
// in the Software without restriction, including without limitation the rights
// to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
// copies of the Software, and to permit persons to whom the Software is
// furnished to do so, subject to the following conditions:
//
// The above copyright notice and this permission notice shall be included in
// all  copies or substantial portions of the Software.
//
// THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
// IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
// FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
// AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
// LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
// OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
// SOFTWARE.

#pragma once

#include "lib/random_shuffle.h"
#include "lib/sparse_table.h"
#include "lib/speculative_for.h"
#include "ligra.h"

namespace MIS_rootset {

template <template <class W> class vertex, class W, class Fl>
auto verify_mis(graph<vertex<W>>& GA, Fl& in_mis) {
  auto d = array_imap<uintE>(GA.n, (uintE)0);
  auto map_f = [&](const uintE& src, const uintE& ngh, const W& wgh) {
    if (!d[ngh]) {
      d[ngh] = 1;
    }
  };
  parallel_for_bc(i, 0, GA.n, true, {
    if (in_mis[i]) {
      GA.V[i].mapOutNgh(i, map_f);
    }
  });
  bool ok = true;
  parallel_for_bc(i, 0, GA.n, true, {
    if (in_mis[i]) {
      assert(!d[i]);
    }
  });
  auto mis_int =
      make_in_imap<size_t>(GA.n, [&](size_t i) { return (size_t)in_mis[i]; });
  size_t mis_size = pbbs::reduce_add(mis_int);
  assert(pbbs::reduce_add(d) == (GA.n - mis_size));
  cout << "MIS Ok" << endl;
}

template <template <class W> class vertex, class W, class VS, class P>
auto get_nghs(graph<vertex<W>>& GA, VS& vs, P p) {
  size_t n = GA.n;
  vs.toSparse();
  assert(!vs.isDense);
  size_t m = vs.size();
  auto deg_im = make_in_imap<size_t>(
      vs.size(), [&](size_t i) { return GA.V[vs.vtx(i)].getOutDegree(); });
  size_t sum_d = pbbs::reduce_add(deg_im);

  if (sum_d > GA.m / 100) {  // dense forward case
    auto dense = array_imap<bool>(GA.n, false);
    auto map_f = [&](const uintE& src, const uintE& ngh, const W& wgh) {
      if (p(ngh) && !dense[ngh]) {
        dense[ngh] = 1;
      }
    };
    parallel_for_bc(i, 0, vs.size(), true, {
      uintE v = vs.vtx(i);
      GA.V[v].mapOutNgh(v, map_f);
    });
    return vertexSubset(GA.n, dense.get_array());
  } else {  // sparse --- iterate, and add nghs satisfying P to a hashtable
    auto ht = make_sparse_table<uintE, pbbs::empty>(
        sum_d, make_tuple(UINT_E_MAX, pbbs::empty()),
        [&](const uintE& k) { return pbbs::hash64(k); });
    vs.toSparse();
    parallel_for_bc(i, 0, vs.size(), true, {
      auto map_f = [&](const uintE& src, const uintE& ngh, const W& wgh) {
        if (p(ngh)) {
          ht.insert(make_tuple(ngh, pbbs::empty()));
        }
      };
      uintE v = vs.vtx(i);
      GA.V[v].mapOutNgh(v, map_f);
    });
    auto nghs = ht.entries();
    ht.del();
    return vertexSubset(GA.n, nghs.size(), (uintE*)nghs.get_array());
  }
}

inline bool hash_lt(const uintE& src, const uintE& ngh) {
  uint32_t src_h = pbbs::hash32(src);
  uint32_t ngh_h = pbbs::hash32(ngh);
  return (src_h < ngh_h) || ((src_h == ngh_h) && src < ngh);
};

template <class W>
struct mis_f {
  intE* p;
  uintE* perm;
  mis_f(intE* _p, uintE* _perm) : p(_p), perm(_perm) {}
  inline bool update(const uintE& s, const uintE& d, const W& w) {
    if (perm[s] < perm[d]) {
      p[d]--;
      return p[d] == 0;
    }
    return false;
  }
  inline bool updateAtomic(const uintE& s, const uintE& d, const W& wgh) {
    if (perm[s] < perm[d]) {
      return (pbbs::xadd(&p[d], -1) == 1);
    }
    return false;
  }
  inline bool cond(uintE d) { return (p[d] > 0); }
};

template <class W>
struct mis_f_2 {
  intE* p;
  mis_f_2(intE* _p) : p(_p) {}
  inline bool update(const uintE& s, const uintE& d, const W& w) {
    if (hash_lt(s, d)) {
      p[d]--;
      return p[d] == 0;
    }
    return false;
  }
  inline bool updateAtomic(const uintE& s, const uintE& d, const W& wgh) {
    if (hash_lt(s, d)) {
      return (writeAdd(&p[d], -1) == 0);
    }
    return false;
  }
  inline bool cond(uintE d) { return (p[d] > 0); }  // still live
};

template <template <class W> class vertex, class W>
auto MIS(graph<vertex<W>>& GA) {
  timer init_t;
  init_t.start();
  size_t n = GA.n;

  // compute the priority DAG
  auto priorities = array_imap<intE>(n);
  auto perm = pbbs::random_permutation<uintE>(n);
  parallel_for_bc(i, 0, n, true, {
    uintE our_pri = perm[i];
    auto count_f = wrap_f<W>([&](uintE src, uintE ngh) {
      uintE ngh_pri = perm[ngh];
      return ngh_pri < our_pri;
    });
    priorities[i] = GA.V[i].countOutNgh(i, count_f);
  });

  // compute the initial rootset
  auto zero_map =
      make_in_imap<bool>(n, [&](size_t i) { return priorities[i] == 0; });
  auto init = pbbs::pack_index<uintE>(zero_map);
  auto roots = vertexSubset(n, init.size(), init.get_array());

  auto in_mis = array_imap<bool>(n, false);
  size_t finished = 0;
  size_t rounds = 0;
  init_t.stop();
  init_t.reportTotal("init");
  while (finished != n) {
    assert(roots.size() > 0);
    cout << "round = " << rounds << " size = " << roots.size()
         << " remaining = " << (n - finished) << endl;

    // set the roots in the MIS
    vertexMap(roots, [&](uintE v) { in_mis[v] = true; });

    // compute neighbors of roots that are still live
    auto removed = get_nghs(
        GA, roots, [&](const uintE& ngh) { return priorities[ngh] > 0; });
    vertexMap(removed, [&](uintE v) { priorities[v] = 0; });

    // compute the new roots: neighbors of removed that have their priorities
    // set to 0 after eliminating all nodes in removed
    intE* pri = priorities.start();
    timer nr;
    nr.start();
    auto new_roots =
        edgeMap(GA, removed, mis_f<W>(pri, perm.start()), -1, sparse_blocked);
    nr.stop();
    nr.reportTotal("new roots time");

    // update finished with roots and removed. update roots.
    finished += roots.size();
    finished += removed.size();
    removed.del();
    roots.del();

    roots = new_roots;
    rounds++;
  }
  return std::move(in_mis);
}
}  // namespace MIS_rootset

namespace MIS_spec_for {
// For each vertex:
//   Flags = 0 indicates undecided
//   Flags = 1 indicates chosen
//   Flags = 2 indicates a neighbor is chosen
template <template <class W> class vertex, class W>
struct MISstep {
  char* FlagsNext;
  char* Flags;
  graph<vertex<W>>& G;

  MISstep(char* _PF, char* _F, graph<vertex<W>>& _G)
      : FlagsNext(_PF), Flags(_F), G(_G) {}

  bool reserve(intT i) {
    // decode neighbor
    FlagsNext[i] = 1;

    auto map_f =
        wrap_f<W>([&](const uintE& src, const uintE& ngh) -> tuple<int, int> {
          if (ngh < src) {
            auto fl = Flags[ngh];
            return make_tuple(fl == 1, fl == 0);
          }
          return make_tuple(0, 0);
        });
    auto red_f = [&](const tuple<int, int>& l, const tuple<int, int>& r) {
      return make_tuple(get<0>(l) + get<0>(r), get<1>(l) + get<1>(r));
    };
    auto id = make_tuple(0, 0);
    auto res = G.V[i].reduceOutNgh(i, id, map_f, red_f);
    if (get<0>(res) > 0) {
      FlagsNext[i] = 2;
    } else if (get<1>(res) > 0) {
      FlagsNext[i] = 0;
    }
    return 1;
  }

  bool commit(intT i) { return (Flags[i] = FlagsNext[i]) > 0; }
};

template <template <class W> class vertex, class W>
auto MIS(graph<vertex<W>>& GA) {
  size_t n = GA.n;
  auto Flags = array_imap<char>(n, [&](size_t i) { return 0; });
  auto FlagsNext = array_imap<char>(n);
  auto mis = MISstep<vertex, W>(FlagsNext.start(), Flags.start(), GA);
  eff_for<uintE>(mis, 0, n, 50);
  return std::move(Flags);
}
};  // namespace MIS_spec_for

template <template <class W> class vertex, class W, class Seq>
auto verify_MIS(graph<vertex<W>>& GA, Seq& mis) {
  size_t n = GA.n;
  auto ok = array_imap<bool>(n, [&](size_t i) { return 1; });
  parallel_for_bc(i, 0, n, true, {
    auto pred = [&](const uintE& src, const uintE& ngh, const W& wgh) {
      return mis[ngh];
    };
    size_t ct = GA.V[i].countOutNgh(i, pred);
    ok[i] = (mis[i]) ? (ct == 0) : (ct > 0);
  });
  auto ok_imap = make_in_imap<size_t>(n, [&](size_t i) { return ok[i]; });
  size_t n_ok = pbbs::reduce_add(ok_imap);
  if (n_ok == n) {
    cout << "valid MIS" << endl;
  } else {
    cout << "invalid MIS, " << (n - n_ok) << " vertices saw bad neighborhoods"
         << endl;
  }
}
// Adapted from
// https://github.com/paulmach/orb/blob/dcade4901baea0727377ccf7c4aab2addd92d152/simplify/visvalingam.go

// The MIT License (MIT)
//
// Copyright (c) 2017 Paul Mach
//
// Permission is hereby granted, free of charge, to any person obtaining
// a copy of this software and associated documentation files (the
// "Software"), to deal in the Software without restriction, including
// without limitation the rights to use, copy, modify, merge, publish,
// distribute, sublicense, and/or sell copies of the Software, and to
// permit persons to whom the Software is furnished to do so, subject
// to the following conditions:
//
// The above copyright notice and this permission notice shall be
// included in all copies or substantial portions of the Software.
//
// THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,
// EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
// MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.
// IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR
// ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF
// CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION
// WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.

#include <vector>
#include <cmath>
#include "clipper.h"

// Stuff to create the priority queue, or min heap.
// Rewriting it here, vs using the std lib, resulted in a 50% performance bump!

struct visItem {
  double area = 0;     // triangle area
  int ptidx = 0;  // index of point in original path

  // to keep a virtual linked list to help rebuild the triangle areas as we remove points.
  visItem *next = NULL;
  visItem *prev = NULL;

  int index = 0;  // internal index in heap, for removal and update
};

struct minHeap {
  std::vector<visItem *> h;

  void Push(visItem *item) {
    item->index = h.size();
    h.push_back(item);
    up(item->index);
  }

  visItem *Pop() {
    visItem *removed = h[0];
    visItem *lastItem = h[h.size() - 1];
    h.pop_back();

    if (h.size() > 0) {
      lastItem->index = 0;
      h[0] = lastItem;
      down(0);
    }

    return removed;
  }

  void Update(visItem *item, double area) {
    if (item->area > area) {
      // area got smaller
      item->area = area;
      up(item->index);
    } else {
      // area got larger
      item->area = area;
      down(item->index);
    }
  }

  void up(int i) {
    visItem *object = h[i];
    while (i > 0) {
      int up = ((i + 1) >> 1) - 1;
      visItem *parent = h[up];

      if (parent->area <= object->area) {
        // parent is smaller so we're done fixing up the heap.
        break;
      }

      // swap nodes
      parent->index = i;
      h[i] = parent;

      object->index = up;
      h[up] = object;

      i = up;
    }
  }

  void down(int i) {
    visItem *object = h[i];
    while (1) {
      size_t right = (i + 1) << 1;
      size_t left = right - 1;

      int down = i;
      visItem *child = h[down];

      // swap with smallest child
      if (left < h.size() && h[left]->area < child->area) {
        down = left;
        child = h[down];
      }

      if (right < h.size() && h[right]->area < child->area) {
        down = right;
        child = h[down];
      }

      // non smaller, so quit
      if (down == i) {
        break;
      }

      // swap the nodes
      child->index = i;
      h[child->index] = child;

      object->index = down;
      h[down] = object;

      i = down;
    }
  }
};

static double doubleTriangleArea(const std::vector<vt_point>& pts, int i1, int i2, int i3)
{
  auto& a = pts[i1];
  auto& b = pts[i2];
  auto& c = pts[i3];

  return std::abs((b.x - a.x) * (c.y - a.y) - (b.y - a.y) * (c.x - a.x));
}

std::vector<int> visvalingam(const std::vector<vt_point>& pts, real thresh)
{
  if(thresh <= 0 || pts.size() < 3) { return {}; }
  // edge cases checked, get on with it
  //int removed = 0;
  thresh *= 2;

  // build the initial minheap linked list.
  minHeap heap;
  heap.h.reserve(pts.size());

  visItem linkedListStart;
  linkedListStart.area = INFINITY;
  linkedListStart.ptidx = 0;
  heap.Push(&linkedListStart);

  // internal path items
  std::vector<visItem> items;
  items.resize(pts.size());

  {
    visItem *prev = &linkedListStart;
    for (size_t i = 1; i < pts.size() - 1; i++) {
      visItem *item = &items[i];

      item->area = doubleTriangleArea(pts, i - 1, i, i + 1);
      item->ptidx = i;
      item->prev = prev;

      heap.Push(item);
      prev->next = item;
      prev = item;
    }

    // final item
    visItem *endItem = &items.back();  //items[pts.size() - 1];
    endItem->area = INFINITY;
    endItem->ptidx = pts.size() - 1;  //(end - start) - 1;
    endItem->prev = prev;

    prev->next = endItem;
    heap.Push(endItem);
  }

  // run through the reduction process
  while (heap.h.size() > 0) {
    visItem *current = heap.Pop();
    if (current->area > thresh) {  // || (end - start) - removed <= retain) {
      break;
    }

    visItem *next = current->next;
    visItem *prev = current->prev;

    // remove current element from linked list
    prev->next = current->next;
    next->prev = current->prev;
    //removed++;

    // figure out the new areas
    if (prev->prev != NULL) {
      double area = doubleTriangleArea(pts, prev->prev->ptidx, prev->ptidx, next->ptidx);
      area = std::max(area, current->area);
      heap.Update(prev, area);
    }

    if (next->next != NULL) {
      double area = doubleTriangleArea(pts, prev->ptidx, next->ptidx, next->next->ptidx);
      area = std::max(area, current->area);
      heap.Update(next, area);
    }
  }

  std::vector<int> keep(pts.size(), 0);
  visItem *item = &linkedListStart;
  while (item != NULL) {
    keep[item->ptidx] = 1;
    item = item->next;
  }
  return keep;
}

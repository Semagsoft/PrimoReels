#pragma once

// Generic undo/redo snapshot stacks for project edits. Extracted from
// TimelineEngine so the history mechanics (depth limit, redo invalidation,
// current-state capture) live in one tested place. The engine owns what a
// snapshot contains and what applying one means.

#include <QVector>

template <typename T>
class ProjectHistory {
 public:
  static constexpr int kLimit = 200;

  bool canUndo() const { return !m_undoStack.isEmpty(); }
  bool canRedo() const { return !m_redoStack.isEmpty(); }

  // Record a pre-edit snapshot. Any redo chain is invalidated.
  void push(const T& snapshot) {
    m_undoStack.append(snapshot);
    while (m_undoStack.size() > kLimit) {
      m_undoStack.removeFirst();
    }
    m_redoStack.clear();
  }

  // Move the top undo entry into the redo chain (after recording `current`
  // there) and return it via `out`. False when nothing to undo.
  bool undo(const T& current, T* out) {
    if (m_undoStack.isEmpty()) {
      return false;
    }
    m_redoStack.append(current);
    *out = m_undoStack.takeLast();
    return true;
  }

  // Symmetric: record `current` on the undo stack, return the redo entry.
  bool redo(const T& current, T* out) {
    if (m_redoStack.isEmpty()) {
      return false;
    }
    m_undoStack.append(current);
    *out = m_redoStack.takeLast();
    return true;
  }

  void clear() {
    m_undoStack.clear();
    m_redoStack.clear();
  }

 private:
  QVector<T> m_undoStack;
  QVector<T> m_redoStack;
};

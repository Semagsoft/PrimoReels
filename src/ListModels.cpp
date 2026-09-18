#include "ListModels.h"

#include <QSet>

namespace {

// First/last row where a and b differ (same-size lists, known to differ).
void firstLastDiff(const QVariantList& a, const QVariantList& b, int& first, int& last) {
  const int n = a.size();
  first = 0;
  while (first < n && a.at(first) == b.at(first)) {
    ++first;
  }
  last = n - 1;
  while (last > first && a.at(last) == b.at(last)) {
    --last;
  }
}

// newList == oldList with exactly one row inserted at *pos (or -1).
int singleInsertPos(const QVariantList& oldList, const QVariantList& newList) {
  if (newList.size() != oldList.size() + 1) {
    return -1;
  }
  int f = 0;
  while (f < oldList.size() && oldList.at(f) == newList.at(f)) {
    ++f;
  }
  for (int i = f; i < oldList.size(); ++i) {
    if (oldList.at(i) != newList.at(i + 1)) {
      return -1;
    }
  }
  return f;
}

// newList == oldList with exactly the row at *pos removed (or -1).
int singleRemovePos(const QVariantList& oldList, const QVariantList& newList) {
  if (oldList.size() != newList.size() + 1) {
    return -1;
  }
  int f = 0;
  while (f < newList.size() && oldList.at(f) == newList.at(f)) {
    ++f;
  }
  for (int i = f; i < newList.size(); ++i) {
    if (oldList.at(i + 1) != newList.at(i)) {
      return -1;
    }
  }
  return f;
}

// newList == oldList with the single row at from moved to to.
// Verifies by reconstruction, so duplicate rows cannot confuse it.
bool singleMove(const QVariantList& oldList, const QVariantList& newList, int& from, int& to) {
  const int n = oldList.size();
  if (n != newList.size() || n < 2) {
    return false;
  }
  int f = 0;
  while (f < n && oldList.at(f) == newList.at(f)) {
    ++f;
  }
  int l = n - 1;
  while (l > f && oldList.at(l) == newList.at(l)) {
    --l;
  }
  // Forward move: old[f] lands at l, the middle shifts left.
  bool forward = newList.at(l) == oldList.at(f);
  for (int i = f; forward && i < l; ++i) {
    forward = newList.at(i) == oldList.at(i + 1);
  }
  if (forward) {
    from = f;
    to = l;
    return true;
  }
  // Backward move: old[l] lands at f, the middle shifts right.
  bool backward = newList.at(f) == oldList.at(l);
  for (int i = f + 1; backward && i <= l; ++i) {
    backward = newList.at(i) == oldList.at(i - 1);
  }
  if (backward) {
    from = l;
    to = f;
    return true;
  }
  return false;
}

QString itemPath(const QVariant& v) {
  return v.toMap().value(QStringLiteral("path")).toString();
}

void pruneTicks(QHash<QString, int>& ticks, const QVariantList& items) {
  if (ticks.isEmpty()) {
    return;
  }
  QSet<QString> live;
  for (const QVariant& v : items) {
    live.insert(itemPath(v));
  }
  for (auto it = ticks.begin(); it != ticks.end();) {
    if (!live.contains(it.key())) {
      it = ticks.erase(it);
    } else {
      ++it;
    }
  }
}

}  // namespace

// ---- MediaListModel ----

MediaListModel::MediaListModel(QObject* parent) : QAbstractListModel(parent) {}

int MediaListModel::rowCount(const QModelIndex& parent) const {
  if (parent.isValid()) {
    return 0;
  }
  return m_items.size();
}

QVariant MediaListModel::data(const QModelIndex& index, int role) const {
  if (!index.isValid() || index.row() < 0 || index.row() >= m_items.size()) {
    return {};
  }
  const QVariantMap m = m_items.at(index.row()).toMap();
  switch (role) {
    case PathRole:
      return m.value(QStringLiteral("path"));
    case NameRole:
      return m.value(QStringLiteral("name"));
    case DurationRole:
      return m.value(QStringLiteral("duration"), 0.0);
    case ThumbTickRole:
      return m_thumbTicks.value(itemPath(m_items.at(index.row())), 0);
    default:
      return {};
  }
}

QHash<int, QByteArray> MediaListModel::roleNames() const {
  return {
      {PathRole, "path"},
      {NameRole, "name"},
      {DurationRole, "duration"},
      {ThumbTickRole, "thumbTick"},
  };
}

QVariantMap MediaListModel::get(int row) const {
  if (row < 0 || row >= m_items.size()) {
    return {};
  }
  return m_items.at(row).toMap();
}

int MediaListModel::indexOfPath(const QString& path) const {
  for (int i = 0; i < m_items.size(); ++i) {
    if (itemPath(m_items.at(i)) == path) {
      return i;
    }
  }
  return -1;
}

void MediaListModel::rebuildIndex() {
  m_pathRows.clear();
  m_pathRows.reserve(m_items.size() * 2 + 1);
  for (int i = 0; i < m_items.size(); ++i) {
    m_pathRows[itemPath(m_items.at(i))].append(i);
  }
}

void MediaListModel::setItems(const QVariantList& items) {
  static const QList<int> kRoles = {PathRole, NameRole, DurationRole, ThumbTickRole};
  if (m_items == items) {
    return;
  }
  if (m_setItemsInProgress) {
    return;
  }
  // RAII guard: sets the flag on construction and resets it on all exit
  // paths (exceptions, early returns), so the flag can never be observed
  // set without the guard active.
  struct Guard {
    bool* flag;
    Guard(bool* f) : flag(f) { *flag = true; }
    ~Guard() { *flag = false; }
  };
  Guard guard{&m_setItemsInProgress};
  if (const int pos = singleInsertPos(m_items, items); pos >= 0) {
    beginInsertRows(QModelIndex(), pos, pos);
    m_items = items;
    rebuildIndex();
    endInsertRows();
    return;
  }
  if (const int pos = singleRemovePos(m_items, items); pos >= 0) {
    beginRemoveRows(QModelIndex(), pos, pos);
    m_items = items;
    pruneTicks(m_thumbTicks, m_items);
    rebuildIndex();
    endRemoveRows();
    return;
  }
  if (items.size() == m_items.size()) {
    int from = -1, to = -1;
    if (singleMove(m_items, items, from, to)) {
      // Destination numbering: after removal for upward moves (+1 past the
      // target for downward moves, per beginMoveRows contract).
      beginMoveRows(QModelIndex(), from, from, QModelIndex(), from < to ? to + 1 : to);
      m_items = items;
      rebuildIndex();
      endMoveRows();
      return;
    }
    int first = 0, last = 0;
    firstLastDiff(m_items, items, first, last);
    m_items = items;
    rebuildIndex();
    emit dataChanged(index(first, 0), index(last, 0), kRoles);
    return;
  }
  if (items.isEmpty()) {
    beginRemoveRows(QModelIndex(), 0, m_items.size() - 1);
    m_items.clear();
    m_thumbTicks.clear();
    m_pathRows.clear();
    endRemoveRows();
    return;
  }
  beginResetModel();
  m_items = items;
  pruneTicks(m_thumbTicks, m_items);
  rebuildIndex();
  endResetModel();
}

void MediaListModel::bumpThumb(const QString& path) {
  if (path.isEmpty()) {
    return;
  }
  m_thumbTicks[path]++;
  const auto rows = m_pathRows.value(path);
  for (int i : rows) {
    emit dataChanged(index(i, 0), index(i, 0), {ThumbTickRole});
  }
}

// ---- TimelineClipModel ----

TimelineClipModel::TimelineClipModel(QObject* parent) : QAbstractListModel(parent) {}

int TimelineClipModel::rowCount(const QModelIndex& parent) const {
  if (parent.isValid()) {
    return 0;
  }
  return m_items.size();
}

QVariant TimelineClipModel::data(const QModelIndex& index, int role) const {
  if (!index.isValid() || index.row() < 0 || index.row() >= m_items.size()) {
    return {};
  }
  const QVariantMap m = m_items.at(index.row()).toMap();
  const QString path = m.value(QStringLiteral("path")).toString();
  switch (role) {
    case PathRole:
      return m.value(QStringLiteral("path"));
    case NameRole:
      return m.value(QStringLiteral("name"));
    case DurationRole:
      return m.value(QStringLiteral("duration"), 0.0);
    case SourceDurationRole:
      return m.value(QStringLiteral("sourceDuration"), m.value(QStringLiteral("duration"), 0.0));
    case TrimStartRole:
      return m.value(QStringLiteral("trimStart"), 0.0);
    case TrimEndRole:
      return m.value(QStringLiteral("trimEnd"), 0.0);
    case EffectRole:
      return m.value(QStringLiteral("effect"), QString());
    case TransitionRole:
      return m.value(QStringLiteral("transition"), QString());
    case TransitionDurationRole:
      return m.value(QStringLiteral("transitionDuration"), 0.5);
    case TrackRole:
      return m.value(QStringLiteral("track"), 0);
    case TitleRole:
      return m.value(QStringLiteral("title"), QString());
    case StartTimeRole:
      return m.value(QStringLiteral("startTime"), 0.0);
    case GainRole:
      return m.value(QStringLiteral("gain"), 1.0);
    case MutedRole:
      return m.value(QStringLiteral("muted"), false);
    case FadeInRole:
      return m.value(QStringLiteral("fadeIn"), 0.0);
    case FadeOutRole:
      return m.value(QStringLiteral("fadeOut"), 0.0);
    case ThumbTickRole:
      return m_thumbTicks.value(path, 0);
    case WaveTickRole:
      return m_waveTicks.value(path, 0);
    default:
      return {};
  }
}

QHash<int, QByteArray> TimelineClipModel::roleNames() const {
  return {
      {PathRole, "path"},
      {NameRole, "name"},
      {DurationRole, "duration"},
      {SourceDurationRole, "sourceDuration"},
      {TrimStartRole, "trimStart"},
      {TrimEndRole, "trimEnd"},
      {EffectRole, "effect"},
      {TransitionRole, "transition"},
      {TransitionDurationRole, "transitionDuration"},
      {TrackRole, "track"},
      {TitleRole, "title"},
      {StartTimeRole, "startTime"},
      {GainRole, "gain"},
      {MutedRole, "muted"},
      {FadeInRole, "fadeIn"},
      {FadeOutRole, "fadeOut"},
      {ThumbTickRole, "thumbTick"},
      {WaveTickRole, "waveTick"},
  };
}

QVariantMap TimelineClipModel::get(int row) const {
  if (row < 0 || row >= m_items.size()) {
    return {};
  }
  return m_items.at(row).toMap();
}

void TimelineClipModel::rebuildIndex() {
  m_pathRows.clear();
  m_pathRows.reserve(m_items.size() * 2 + 1);
  for (int i = 0; i < m_items.size(); ++i) {
    m_pathRows[itemPath(m_items.at(i))].append(i);
  }
}

void TimelineClipModel::setItems(const QVariantList& items) {
  static const QList<int> kRoles = {PathRole,           NameRole,       DurationRole,
                                    SourceDurationRole, TrimStartRole,  TrimEndRole,
                                    EffectRole,         TransitionRole, TransitionDurationRole,
                                    TrackRole,          TitleRole,      StartTimeRole,
                                    GainRole,           MutedRole,      FadeInRole,
                                    FadeOutRole,        ThumbTickRole,  WaveTickRole};
  if (m_items == items) {
    return;
  }
  if (m_setItemsInProgress) {
    return;
  }
  // RAII guard: sets the flag on construction and resets it on all exit
  // paths (exceptions, early returns), so the flag can never be observed
  // set without the guard active.
  struct Guard {
    bool* flag;
    Guard(bool* f) : flag(f) { *flag = true; }
    ~Guard() { *flag = false; }
  };
  Guard guard{&m_setItemsInProgress};
  if (const int pos = singleInsertPos(m_items, items); pos >= 0) {
    beginInsertRows(QModelIndex(), pos, pos);
    m_items = items;
    rebuildIndex();
    endInsertRows();
    return;
  }
  if (const int pos = singleRemovePos(m_items, items); pos >= 0) {
    beginRemoveRows(QModelIndex(), pos, pos);
    m_items = items;
    pruneTicks(m_thumbTicks, m_items);
    pruneTicks(m_waveTicks, m_items);
    rebuildIndex();
    endRemoveRows();
    return;
  }
  if (items.size() == m_items.size()) {
    int from = -1, to = -1;
    if (singleMove(m_items, items, from, to)) {
      beginMoveRows(QModelIndex(), from, from, QModelIndex(), from < to ? to + 1 : to);
      m_items = items;
      rebuildIndex();
      endMoveRows();
      return;
    }
    int first = 0, last = 0;
    firstLastDiff(m_items, items, first, last);
    m_items = items;
    rebuildIndex();
    emit dataChanged(index(first, 0), index(last, 0), kRoles);
    return;
  }
  if (items.isEmpty()) {
    beginRemoveRows(QModelIndex(), 0, m_items.size() - 1);
    m_items.clear();
    m_thumbTicks.clear();
    m_waveTicks.clear();
    m_pathRows.clear();
    endRemoveRows();
    return;
  }
  beginResetModel();
  m_items = items;
  pruneTicks(m_thumbTicks, m_items);
  pruneTicks(m_waveTicks, m_items);
  rebuildIndex();
  endResetModel();
}

void TimelineClipModel::bumpThumb(const QString& path) {
  if (path.isEmpty()) {
    return;
  }
  m_thumbTicks[path]++;
  const auto rows = m_pathRows.value(path);
  for (int i : rows) {
    emit dataChanged(index(i, 0), index(i, 0), {ThumbTickRole});
  }
}

void TimelineClipModel::bumpWave(const QString& path) {
  if (path.isEmpty()) {
    return;
  }
  m_waveTicks[path]++;
  const auto rows = m_pathRows.value(path);
  for (int i : rows) {
    emit dataChanged(index(i, 0), index(i, 0), {WaveTickRole});
  }
}

// ---- MediaFilterModel ----

MediaFilterModel::MediaFilterModel(QObject* parent) : QSortFilterProxyModel(parent) {
  setFilterCaseSensitivity(Qt::CaseInsensitive);
  setFilterRole(MediaListModel::NameRole);
}

void MediaFilterModel::setFilterText(const QString& text) {
  if (m_filterText == text) {
    return;
  }
  beginFilterChange();
  m_filterText = text;
  endFilterChange();
  emit filterTextChanged();
}

bool MediaFilterModel::filterAcceptsRow(int sourceRow, const QModelIndex& sourceParent) const {
  if (m_filterText.isEmpty()) {
    return true;
  }
  const QModelIndex idx = sourceModel()->index(sourceRow, 0, sourceParent);
  const QString name = sourceModel()->data(idx, MediaListModel::NameRole).toString();
  return name.contains(m_filterText, Qt::CaseInsensitive);
}

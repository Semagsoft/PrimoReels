#pragma once

#include <QAbstractListModel>
#include <QHash>
#include <QSortFilterProxyModel>
#include <QStringList>
#include <QVariantList>
#include <QVariantMap>
#include <cstdint>

// Row-backed models mirroring TimelineEngine's QVariantList truth.
// Engine keeps QVariantList as source of truth (tests/export/undo compat)
// and pushes snapshots here; QML views bind to these models so delegates
// recycle instead of being fully recreated on every JS-array change.
//
// thumbTick/waveTick roles bump per-path when async thumbnails/waveforms
// arrive, so only the affected delegates reload/repaint (the global
// thumbVersion/waveVersion counters remain for compat).

class MediaListModel : public QAbstractListModel {
  Q_OBJECT
 public:
  enum Roles : std::uint16_t {
    PathRole = Qt::UserRole + 1,
    NameRole,
    DurationRole,
    ThumbTickRole,
  };

  explicit MediaListModel(QObject* parent = nullptr);

  int rowCount(const QModelIndex& parent = QModelIndex()) const override;
  QVariant data(const QModelIndex& index, int role = Qt::DisplayRole) const override;
  QHash<int, QByteArray> roleNames() const override;

  Q_INVOKABLE QVariantMap get(int row) const;
  Q_INVOKABLE int indexOfPath(const QString& path) const;

  void setItems(const QVariantList& items);
  // Notify delegates showing this path that its thumbnail arrived.
  void bumpThumb(const QString& path);

 private:
  QVariantList m_items;
  // Per-path arrival counters (pruned to paths present in m_items).
  QHash<QString, int> m_thumbTicks;
  // Path -> rows index rebuilt on setItems; makes bumpThumb O(matches).
  QHash<QString, QList<int>> m_pathRows;
  bool m_setItemsInProgress = false;  // Reentrancy guard
  void rebuildIndex();
};

class TimelineClipModel : public QAbstractListModel {
  Q_OBJECT
 public:
  enum Roles : std::uint16_t {
    PathRole = Qt::UserRole + 1,
    NameRole,
    DurationRole,
    SourceDurationRole,
    TrimStartRole,
    TrimEndRole,
    EffectRole,
    TransitionRole,
    TransitionDurationRole,
    TrackRole,
    TitleRole,
    StartTimeRole,
    GainRole,
    MutedRole,
    FadeInRole,
    FadeOutRole,
    ThumbTickRole,
    WaveTickRole,
  };

  explicit TimelineClipModel(QObject* parent = nullptr);

  int rowCount(const QModelIndex& parent = QModelIndex()) const override;
  QVariant data(const QModelIndex& index, int role = Qt::DisplayRole) const override;
  QHash<int, QByteArray> roleNames() const override;

  Q_INVOKABLE QVariantMap get(int row) const;

  void setItems(const QVariantList& items);
  // Notify delegates showing this path that its thumbnail/waveform arrived.
  void bumpThumb(const QString& path);
  void bumpWave(const QString& path);

 private:
  QVariantList m_items;
  QHash<QString, int> m_thumbTicks;
  QHash<QString, int> m_waveTicks;
  // Path -> rows index rebuilt on setItems; makes bump* O(matches).
  QHash<QString, QList<int>> m_pathRows;
  bool m_setItemsInProgress = false;  // Reentrancy guard
  void rebuildIndex();
};

class MediaFilterModel : public QSortFilterProxyModel {
  Q_OBJECT
  Q_PROPERTY(QString filterText READ filterText WRITE setFilterText NOTIFY filterTextChanged)
 public:
  explicit MediaFilterModel(QObject* parent = nullptr);

  QString filterText() const { return m_filterText; }
  void setFilterText(const QString& text);

 signals:
  void filterTextChanged();

 protected:
  bool filterAcceptsRow(int sourceRow, const QModelIndex& sourceParent) const override;

 private:
  QString m_filterText;
};

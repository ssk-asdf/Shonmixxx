#pragma once

#include <QPointer>

#include "preferences/usersettings.h"
#include "track/track_decl.h"
#include "widget/wwidget.h"

class QLabel;
class QTimer;
class Library;

/// ShonDJ: the space next to the library cover. Shows the selected song's
/// BPM and key, tonight's crate, what Away plays next and waiting requests.
class ShonInfoPanel : public WWidget {
    Q_OBJECT
  public:
    ShonInfoPanel(QWidget* pParent, UserSettingsPointer pConfig, Library* pLibrary);

  public slots:
    void slotTrackSelected(TrackPointer pTrack);

  private:
    void refresh();

    UserSettingsPointer m_pConfig;
    QPointer<Library> m_pLibrary;
    TrackPointer m_pTrack;
    QLabel* m_pTitle;
    QLabel* m_pBpmKey;
    QLabel* m_pTonight;
    QLabel* m_pUpNext;
    QLabel* m_pRequests;
    QTimer* m_pTimer;
};

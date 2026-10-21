import QtQuick

QtObject {
    id: root
    property QuickView quickView: QuickView {}
    property Dashboard dashboard: Dashboard {}
    property SettingsWindow preferences: SettingsWindow {}
    property Connections routing: Connections {
        target: desktop
        function onWindowRequested(page) {
            var target = page === "settings" ? root.preferences :
                page === "quick-view" ? root.quickView : root.dashboard;
            if (page === "usage" || page === "spending" || page === "sessions")
                root.dashboard.selectedTab = page === "spending" ? 2 : (page === "sessions" ? 1 : 0);
            target.show();
            target.raise();
            target.requestActivate();
        }
    }
}

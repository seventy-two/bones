// The window's contents: a sidebar of pinned folders, drives and WSL
// distros; a folder as thumbnails or as sortable details; and the usual
// file things (open, copy, paste, rename, recycle, drag out).
#pragma once

#include <windows.h>
#include <d3d11.h>

#include <atomic>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "fs.h"
#include "paths.h"
#include "theme.h"
#include "thumbs.h"

namespace shelf {

class App {
public:
    App(HWND hwnd, ID3D11Device* device, std::function<void()> wake);
    ~App();

    void Frame();
    // Pump takes in finished work and runs what can't run mid-frame
    // (drags, the shell's menu); call every loop, outside a frame.
    void Pump();
    // Busy asks for a frame a second (loading, a message to clear).
    bool Busy() const { return loading_ || !message_.empty(); }

    // InCaption says whether a point (client pixels) drags the window.
    bool InCaption(float x, float y) const;
    void SetScale(float scale);
    void Save();
    // Placement reads the saved window position (false if none).
    static bool Placement(RECT& r, bool& maximised);

    // Start opens the first folder: the one given, else where it was
    // left, else home.
    void Start(const std::wstring& path);
    void Navigate(std::wstring path, bool record = true);
    void Back();
    void Forward();
    void Dropped(const std::vector<std::wstring>& paths);
    void DrivesChanged();

    // For test runs and screenshots.
    void SetView(bool thumbs);
    void SetSort(SortKey key, bool descending);
    void SelectName(const std::wstring& name);

private:
    struct Pin {
        std::string label;
        std::wstring path;
    };
    struct Visit {
        std::wstring path, focus;
        float scroll = 0;
    };
    struct Listing {
        int gen = 0;
        std::wstring dir;
        std::vector<Entry> entries;
        bool ok = false;
        DWORD error = 0;
    };
    enum class Later { None, Drag, ItemMenu, FolderMenu };

    // Listing and the view.
    void go(std::wstring path, bool record, std::wstring focus, float scroll);
    void list(bool quiet);
    void reindex();
    // setView switches thumbnails/details; manual ones stick per folder.
    void setView(bool thumbs, bool manual);
    void chooseView();
    void took(Listing&& l);
    void rebuild();
    bool hidden(const Entry& e) const;
    std::wstring pathOf(int vi) const;
    const Entry& at(int vi) const { return entries_[view_[vi]]; }
    bool isSelected(int vi) const;
    void selectOnly(int vi);
    void selectRange(int a, int b, bool add);
    void toggle(int vi);
    std::vector<std::wstring> selectedPaths() const;
    std::vector<int> selectedIndices() const;
    int indexOf(const std::wstring& name) const;
    void focusOn(int vi, bool extend);

    // Drawing.
    void drawToolbar();
    void drawCrumbs(ImVec2 at, float width, float height);
    float windowButtons();
    bool iconButton(const char* id, const char* icon, const char* tip, bool enabled, ImVec2 at, ImVec2 size, bool on = false);
    void goTo(const std::string& typed);
    void drawSidebar();
    bool sideItem(const char* icon, const std::string& label, const std::wstring& path, const char* sub = nullptr,
                  float fill = -1, bool warn = false, bool isHome = false);
    void drawHome();
    void drawGrid();
    void drawDetails();
    void drawStatus();
    void drawMenus();
    void itemPressed(int vi);
    void itemReleased(int vi);
    void drawName(int vi, float width, bool centred);
    void drawIcon(int vi, const Entry& e, ImVec2 p0, ImVec2 p1, bool big, int order);
    void drawFolder(int vi, const Entry& e, ImVec2 p0, ImVec2 p1, int order, bool previews);
    void drawEmpty();
    void keys();

    // Actions.
    void open(int vi);
    void openSelection();
    void up();
    void refresh() { list(true); }
    void startRename(int vi);
    void finishRename(bool keep);
    void newFolder();
    void copySelection(bool cut);
    void paste(const std::wstring& into);
    void recycle(bool permanently);
    void copyPaths(bool linux);
    void pin(const std::wstring& path);
    void unpin(const std::wstring& path);
    bool pinned(const std::wstring& path) const;
    void openTerminal();
    void flash(const std::string& text, bool bad = false);
    void load();

    HWND hwnd_;
    ID3D11Device* device_;
    std::function<void()> wake_;
    std::unique_ptr<Thumbs> thumbs_;
    std::unique_ptr<Icons> icons_;
    Fonts fonts_;
    float scale_ = 1;

    // Where we are ("" is This PC).
    std::wstring cur_;
    bool started_ = false;
    std::vector<Visit> back_, fwd_;
    std::wstring arriveFocus_;  // select this once the listing arrives
    float arriveScroll_ = -1;
    float scrollY_ = 0;

    // What threads hand back: listings, drives, and word that the folder
    // changed. Shared, so a thread that outlives the window is harmless.
    struct Shared {
        std::mutex mu;
        std::vector<Listing> listings;
        std::vector<Drive> drives;
        bool drivesNew = false;
        std::atomic<bool> dirty{false};
    };
    std::shared_ptr<Shared> shared_ = std::make_shared<Shared>();
    int gen_ = 0;
    bool loading_ = false;
    ULONGLONG loadStart_ = 0;
    std::string listError_;
    std::vector<Entry> entries_;
    std::vector<std::wstring> types_;  // per entry, for sorting by type
    std::vector<int> view_;            // what shows, in order
    std::vector<std::wstring> lower_;  // per entry, its name in lower case
    std::unordered_map<std::wstring, int> byName_;  // lower-case name to entry
    std::vector<int> viewPos_;         // per entry, where it shows (-1 if not)
    int cols_ = 1, page_ = 10;         // the grid's columns, items a screen
    Watcher watcher_;

    // Selection, by lower-case name so it lasts through a refresh.
    std::unordered_set<std::wstring> selected_;
    int anchor_ = -1, focus_ = -1;
    int pressed_ = -1;       // pressed on an already-selected item: select only it if not dragged
    bool dragArmed_ = false;
    bool scrollToFocus_ = false;
    std::wstring typed_;
    ULONGLONG typedAt_ = 0;
    // Rubber-band selection in the grid.
    bool banding_ = false;
    ImVec2 bandFrom_;
    std::unordered_set<std::wstring> bandBase_;

    // Settings.
    bool thumbView_ = true;
    float tile_ = 150;
    Sort sort_;
    bool sortApplied_ = false;  // the details table has been told the sort
    bool showHidden_ = false;
    bool autoView_ = true;          // thumbnails where it's mostly media
    bool forceView_ = false;        // a test run fixed the view
    bool chooseOnArrival_ = false;  // pick the view when the listing lands
    std::unordered_map<std::wstring, bool> folderViews_;  // picked by hand, by lower-case path
    std::vector<std::wstring> folderViewOrder_;
    std::vector<Pin> pins_;
    // Folders by use, for This PC's links under each drive. A visit counts
    // once you've stayed 2 s, so folders passed through on the way don't.
    std::vector<Use> uses_;
    std::vector<std::wstring> hiddenUses_;  // hidden from those links by hand
    ULONGLONG arrivedAt_ = 0;
    bool visitCounted_ = true;
    void noteVisit();

    // Sidebar data.
    std::vector<Drive> drives_;
    std::vector<Distro> distros_;

    // Editing.
    char filter_[256] = {};
    bool focusFilter_ = false;
    bool editingPath_ = false, focusPath_ = false;
    char pathBuf_[2048] = {};
    std::wstring renaming_;
    char renameBuf_[1024] = {};
    bool renameStart_ = false;
    int renameStem_ = 0;

    float captionH_ = 46;
    std::vector<std::pair<ImVec2, ImVec2>> noDrag_;  // the title bar's controls

    Later later_ = Later::None;
    bool openItemMenu_ = false, openFolderMenu_ = false, openPinMenu_ = false;
    int pinMenuAt_ = -1;
    std::string message_;
    bool messageBad_ = false;
    ULONGLONG messageAt_ = 0;
};

}  // namespace shelf

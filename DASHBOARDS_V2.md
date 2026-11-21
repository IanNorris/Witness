# Witness Dashboards v2: Design

Status: implemented on `claude/dashboard-improvements` (2026-11-21). Decisions below override the original proposal.

## Implementation status

| Area | Where | Tests |
|---|---|---|
| Access hardening (event socket filtered per user, calendar/timeline/review/retag/detection/faces scoped to accessible cameras, group and camera delete clean up mappings, `/group/enum` admin-only, non-admins no longer receive camera group ids) | `EventBroadcaster.h`, `CrowAuth.cpp`, `Database.cpp`, `CrowRoutes_*.cpp` | `EventAccessFilterTests`, `test_dashboard_http.py` |
| Dashboard storage, validation, redaction and merge, revision history (last 10), sharing, access preview | `DashboardDocument.*`, `DashboardStorage.*`, `CrowRoutes_Dashboard.cpp` | `DashboardDocumentTests`, `DashboardStorageTests`, `test_dashboard_http.py` |
| Long-session accounts (400-day cookie renewed by `/auth/profile`; kiosk refreshes every 6 h) | `CrowRoutes_Auth.cpp`, admin Users page | `test_dashboard_http.py` |
| `cameras=` clip filter, "Clips for this dashboard" (`/clips/dashboard/:id`) | `CrowRoutes_Clip.cpp`, `ClipsView.vue` | — |
| Client model, spotlight resolver with 1–4 way split, geometry engine, templates, undo history | `types/dashboard.ts`, `utils/canvasEngine.ts`, `utils/dashboardTemplates.ts`, `utils/editorHistory.ts` | `dashboardModel`, `canvasEngine`, `dashboardEditorHistory` tests |
| Renderer, kiosk (`/kiosk/:id`), view (`/d/:id`), editor (`/d/:id/edit`), manager (`/dashboards`), phone list, Overview (`/`) | `components/dashboard/*`, `views/Dashboard*View.vue`, `views/OverviewView.vue` | `fullscreenControls`, `dashboardV2Browser`, `dashboardPlaybackBrowser` |

Run the opt-in HTTP test with
`WITNESS_DASHBOARD_TEST_SERVER=build-vs2026-stable\bin\RelWithDebInfo\WitnessServer.exe python -m unittest discover -s Tools/Tests -p test_dashboard_http.py`.

Not yet done or verified:

- **Real hardware:** not checked on the real wall PC or with live cameras. The browser tests use synthetic cameras and media.
- **Event socket:** the per-user filter on `/ws/events` is unit-tested, but not exercised over a real websocket.
- **Tile options:** a camera tile's `fit: cover` is stored but not offered in the editor (the detection overlay assumes `contain`). The activity tile's grouping and object-focus settings are still the viewer's own toggles inside the strip.
- **Spotlight polish:** no hold-countdown bar on spotlight tiles.
- **Editing:** drag and drop is mouse/pen tested; touch editing on tablets is not.

## Decisions (2026-11-21)

These override anything below that contradicts them.

1. **No legacy migration.** The existing browser-stored layouts are already wrong, so don't import them. Drop section 2.3, `migrateLegacyLayout`, its tests and the `/legacy-dashboard` route. New installs and upgrades start from the Overview and the template gallery.
2. **Auto-focus becomes spotlight tiles.**
   - A dashboard can have **more than one** spotlight tile.
   - A spotlight tile can **subdivide** itself: when several of its sources are active at once, it splits into an equal sub-grid (1, then 2×1/1×2, then 2×2, up to `maxSplit`, default 4). It collapses again as cameras go quiet, each still respecting its hold time.
   - Each source is shown at most once across the dashboard. Spotlights claim cameras in array order.
3. **Admin camera access stays as it is.** Admins see only cameras in their own groups. They can grant themselves access, but may have chosen to hide cameras to keep their views simple.
4. **Personal dashboards are private.** Admins can't see other users' personal dashboards.
5. **Shared dashboards:** recipients can view but not edit. Admins can edit. (Saving a personal copy is still allowed.)
6. **Wall PC:** no dedicated kiosk user. Instead, a user account can carry a **"long session"** flag. Sessions for that account are long-lived and renew on use, so an unattended display stays signed in. Admins set the flag on the Users page.
7. **Phone:** no separate per-user phone list. A phone shows **the camera list of any dashboard** the user can see, in reading order and without the layout. A dashboard switcher lets someone on limited bandwidth move between views. The `list` document kind and its one-per-user index are dropped. Instead, each dashboard can store an optional `mobileOrder` override.
8. **Double-click maximises** a tile in kiosk and dashboard view, returning after the return-to-live timeout or on a second double-click or Esc.
9. **Default look:** rounded corners (8px radius). The gap size is a canvas setting; tune the default once real cameras with mixed resolutions and aspect ratios are on screen. Each camera tile gets a `fit` option (`contain` by default, letterboxed on black).
10. **Clips for this dashboard:** clip search and navigation can be scoped to the current dashboard's cameras via `cameras=`.
11. **Scope:** build the whole design, extras included (extra tiles, revision history), in one pass and then iterate.

Section 6 lists the original open questions; all are now answered above.

Path roots used in citations:

| Root | Folder |
|---|---|
| `UI/` | `web-ui/src/` |
| `SRV/` | `WitnessServer/Source/` |
| `TESTS/` | `web-ui/tests/` |
| `TOOLS/` | `Tools/` |

## 0. Findings that shape the design

### Layouts never reach the server

- Each layout is stored in one browser only, as a JSON blob in localStorage per group: `witness-dashboard-layout-v1-<groupId|all>` (`UI/views/DashboardView.vue:189-191, 277-295`).
- Presets are computed in the browser from that same blob (`UI/types/dashboardLayout.ts:207-230`).
- The server has no dashboard table and no dashboard route.
- Consequences:
  - The wall display's layout exists only in the kiosk browser's profile.
  - An edit made on a desktop cannot reach the kiosk.
- The rest of the dashboard state is also stored per browser:
  - selected group, activity toggle and windowed mode (`DashboardView.vue:37-41, 161`)
  - clock, return-to-live, quiet controls and stream choices (`UI/stores/settings.ts:6-24`)

### Why the current editor is hard to use

- Every camera in the group must have a rectangle.
- Adding a camera drops the custom arrangement back to the default grid (`dashboardLayout.ts:135-136, 175`). The next save then overwrites the custom layout (`DashboardView.vue:254, 293-295, 387`).
- Resizing uses a single bottom-right handle.
- Any overlap is rejected and snaps back (`UI/components/camera/CameraGrid.vue:148-199, 291-302`).
- Hidden cameras keep their rectangles, and those still appear in the editor (`DashboardView.vue:222`).
- Auto-focus is a second layout, edited separately and indexed by position (`DashboardView.vue:204-215`, `CameraGrid.vue:103-127`).
- An automatic slot borrows a camera's rectangle via an anchor (`dashboardLayout.ts:15-18, 250-270`).
- The activity panel is either a pixel dock or a rectangle typed in as numbers (`DashboardView.vue:776-798`).
- All of these controls are crammed into one flat toolbar (`DashboardView.vue:759-836`).

### How camera access works today

- A user sees a camera only through `CameraGroupMapping` joined with `UserGroupMapping` (`SRV/Database.cpp:82-100, 530-543`).
- `CrowAuth::IsCameraAuthenticated` enforces this, and admins get no bypass (`SRV/CrowAuth.cpp:129-150`).
- Streams, previews, clip media, DVR, PTZ and capture each check access per camera.
- So a dashboard document cannot grant access to media. The enforcement work is about the dashboard document itself, plus some existing leaks (section 3.5).
- The group tables already act purely as access tables, so no group data needs migrating.

## 1. UX

### 1.1 Concepts and surfaces

**Concepts**

- **Dashboard**: a named arrangement stored on the server, either personal or shared.
- **Tile**: a typed rectangle on the canvas.
- **Badge**: a small floating overlay pinned to a corner, such as the clock.

**Surfaces**

| Surface | Route | What it is |
|---|---|---|
| Overview | `/` when no dashboard is chosen | The current automatic grid plus activity strip. Needs no setup and is not stored. |
| Dashboard view | `/d/:id` | The canvas inside the app shell, letterboxed. |
| Kiosk | `/kiosk/:id` | Full viewport with no app chrome. All state is in the URL, so a reload restores it. |
| Editor | `/d/:id/edit` | Desktop only: at least 1024px wide with a fine pointer. |
| Manager | `/dashboards` | "My dashboards" and "Shared with me". Admins also see "All shared". |
| Phone | `/` on a narrow or coarse-pointer device | An ordered source list with a picker. |

### 1.2 Editor

**Layout.** A full-window workspace with the app sidebar hidden.

| Region | Size | Contents |
|---|---|---|
| Top bar | 48px | Back, title, scope chip, Undo/Redo, Edit/Preview toggle, target resolution, Templates, zoom readout, Save or Publish, overflow menu (Duplicate, Delete, History). |
| Left palette | 280px | Search; Cameras (snapshot, name, status dot, a tick once placed); Smart (Spotlight, Rotation); Widgets (Activity, Clock, Status); an Unplaced tray. |
| Centre stage | flexible | The canvas at the target aspect ratio, scaled to fit, labelled e.g. "1920 × 1080 · 48%". |
| Right inspector | 320px | Settings for the canvas, for the selected tile, or align/distribute tools when several tiles are selected. |

**Edit mode shows snapshots, not live video.** Tiles show `/camera/preview/<id>`, refreshed every 2s, as `ActivityStrip.vue` does. This means:

- no decoder load while editing
- free scaling of the stage
- drag code never touches a mounted `<video>`

**Interactions.** All use pointer events.

| Gesture | Result |
|---|---|
| Drag from the palette to empty space | Places a tile that fills the largest empty rectangle under the cursor. |
| Drag onto the centre of a tile | Swaps the two tiles, or replaces the tile's source. A displaced camera returns to the palette. |
| Drag onto the outer 25% edge zone of a tile | Splits that tile in half. |
| Drag a tile edge or corner (8 handles, 20px hit area) | Resizes. A shared edge moves the neighbouring tiles too; hold Ctrl to resize one tile only. |
| Press Delete, or drag back to the palette | Removes the tile. |
| "Fill gaps" (F) | Grows tiles into the empty space around them. |
| "Auto-arrange" | Lays sources out in the best equal grid. |

**Placement rules**

- Overlaps can never happen. If a drop has no room, the ghost turns red and nothing changes.
- Each camera can appear only once per dashboard, so there is one player per camera.

**Grid and snapping**

- The logical grid is stored per document. The default is 48×48, so equal spans stay 16:9.
- Snapping is every 2 cells by default; hold Alt for 1 cell.
- Edges snap to other tiles' edges and centres, with guides shown while dragging.
- A readout chip shows the tile's size in cells and in target pixels.
- A dot lattice appears every 4 cells; G toggles it.
- Templates that need fifths use a 40×40 grid.
- Minimum tile sizes:
  - camera: 4×4
  - activity: 4 cells on its short side
  - clock: 6×3

**Templates.** A gallery is shown first when you create a new dashboard.

| Family | Templates |
|---|---|
| Equal grids | 1, 2×2, 3×3, 4×4, 5×5, 3×2, 4×3 |
| Hero | 1+5, 1+7, 4+4, 1+12 |
| With activity | right rail, bottom strip |
| Smart | spotlight + rail, rotation wall |
| Portrait | 1×3, 2×4 |

Applying a template re-flows the existing sources in priority order. Any that don't fit go to the Unplaced tray; nothing is dropped.

**Inspector settings**

| Selected | Settings |
|---|---|
| Camera tile | Source; stream (Auto/Main/Sub); label; playback bar. |
| Smart tile | Trigger; ordered sources; may borrow placed cameras; hold seconds; idle behaviour; keep warm. |
| Activity tile | Scope; direction; grouping; object focus. |
| Canvas | Gap; corner radius; target resolution; return-to-live; quiet controls; camera priority; badges. |

**Undo, save and conflicts**

- Undo history is snapshot-based: one entry per gesture, up to 100. Shortcuts are Ctrl+Z and Ctrl+Shift+Z.
- Drafts are autosaved to `localStorage['witness-dashboard-draft-<id>']`.
- Save sends the base revision. If the server returns 409, the user can Reload, Overwrite, or Save as a copy.

**Preview (P)**

- Uses the real renderer with live video inside a frame at the target size, scaled with a CSS transform.
- Target sizes: This window, 1920×1080, 2560×1440, 3840×2160, 3440×1440, 1080×1920, Custom.
- `MsePlayer` measures its on-screen box, so stream selection still matches the displayed size.

**Keyboard**

- Arrows nudge the selected tile; Shift+arrows resize it; Alt switches to the fine step.
- Tab cycles through tiles; Esc cancels the current action or deselects.
- Handles have aria-labels.

**Visual direction**

- Keep the current palette from `main.scss:2-27`: `#0f1117` / `#4a90d9` / `#79beef` / `#edf3fc` / `#a5b6cc`.
- Video surfaces stay black. No blur over video and no looping animations.
- Canvas:
  - Background `#05070b`.
  - Default "card" look: 4px gaps and 8px radius.
  - "Seamless" option: 0–2px gaps and no radius.
- Tile chrome:
  - Name chip at the bottom left: 12px/600 on `rgba(8,12,18,.72)`.
  - REC and stream indicators at the top right.
  - A static 2px amber ring while there is motion.
  - Smart tiles show "AUTO · <camera>" and a thin hold-countdown bar.
- Editor:
  - Workspace background `#0b0f16`.
  - Canvas shadow `0 12px 40px #0008`.
  - Selected tile: 2px `#79beef` outline with 10px handles.
  - Drop preview: 20% accent fill, dashed border, and a verb label (Swap, Split left, Place).
- Unavailable tile: background `#0b1017`, a lock glyph and "Not available for your account".

### 1.3 Kiosk

- **Chrome:** none. The cursor and controls fade after 2.5s idle. Moving the pointer shows a pill with: switcher, Search clips, Sync, Edit (if allowed) and Exit.
- **Per-tile controls:** unchanged.
  - Select a tile to control it; Esc deselects before it exits.
  - A fullscreen click never starts a recording.
  - The PTZ panel is teleported to `<body>`.
- **Live updates:** the server sends `dashboard:changed`. The kiosk refetches and re-lays out without remounting players for cameras that are still shown.
- **Screen wake lock:** acquired, and re-acquired on `visibilitychange`.
- **Disconnects:** a small "Reconnecting" chip instead of the full-screen modal.
- **Auto-reload:** the existing build-hash reload keeps working, because all state is in the route.
- **Errors:** each tile catches its own errors with `onErrorCaptured`, so one failing tile cannot blank the wall.
- **Maximise (proposed):** double-click expands a tile and it returns after the return-to-live timeout. Today double-click exits fullscreen; see question 7.

### 1.4 Phone

- Shows a vertical list of the sources the user chose, in their order.
- Only cards near the viewport stream; the rest show snapshots.
- A "Customize" sheet has two sections:
  - **Shown:** reorder or remove.
  - **Available:** tap to add.
- The list is stored on the server, per user, and filtered like any dashboard.
- A canvas dashboard opened on a phone renders as a read-only stacked list.
- There is no editor on phones: `/d/:id/edit` redirects to `/d/:id`.

### 1.5 Existing features: kept or replaced

| Today | v2 | Status |
|---|---|---|
| A layout per group, plus a group picker | Standalone dashboards with a switcher | Replaced |
| Presets: hero7, balanced8, grid16 | Templates, applied once and then editable | Replaced |
| Automatic slots: priority, hold, fallback, fixed cameras never stolen | Smart tile (`trigger: motion`, `borrowPlaced: false`) using the same resolver rules | Kept, but with its own rectangle |
| Auto-focus: a second whole-canvas layout | Spotlight smart tile (`borrowPlaced: true`) plus manual focus | Replaced; this one is not lossless (question 1) |
| (none) | Rotation | New |
| Camera priority list | Ordered list per smart tile, plus a dashboard default | Kept |
| Activity dock or rectangle | Activity tile; a dock becomes an edge tile | Kept |
| DVR scrub-back, Sync, return-to-live | Reused as-is; automation still freezes during replay | Kept |
| Server-local clock | Clock badge plus a large clock tile | Kept |
| Adaptive streams | Unchanged, with an optional per-tile stream preference; the per-device override still wins | Kept |
| Warm hidden players | Per-smart-tile `warm` flag | Kept, now optional |

### 1.6 Further tile ideas

| Tile | What it shows | Depends on |
|---|---|---|
| Stream health | Sparklines of disconnects, freezes and latency | An access-filtered summary endpoint |
| System status | Cameras online, recording count, disk and retention | The same endpoint |
| Event timeline | Clips from the last N hours for this dashboard's cameras | `/clip/timeline` being access-filtered first |
| Latest detection | The most recent person, vehicle or face crop | Nothing new |
| PTZ viewpoints | Buttons for a camera's PTZ presets | `/ptz/<id>/presets` |
| Text label | A text note | Nothing new |

## 2. Data model

### 2.1 Document (schema 2; the old localStorage blob is version 1)

```jsonc
{
  "schema": 2,
  "kind": "canvas",                           // "canvas" | "list"
  "grid": { "cols": 48, "rows": 48, "gap": 4, "radius": 8 },
  "target": { "width": 1920, "height": 1080, "fit": "stretch" },   // or "contain"
  "tiles": [
    { "id": "t1", "type": "camera", "x": 0, "y": 0, "w": 32, "h": 32,
      "camera": 8, "stream": "auto", "label": true, "playback": true },
    { "id": "t2", "type": "auto", "x": 32, "y": 0, "w": 16, "h": 16,
      "trigger": "motion",                    // "motion" | "rotate"
      "sources": { "kind": "list", "cameras": [15, 14, 13] },   // ordered = priority; or { "kind": "all" }
      "borrowPlaced": false,                  // true = spotlight (swaps with the camera's own tile)
      "holdSeconds": 15,                      // 3..120
      "idle": { "kind": "camera", "camera": 13 },   // | rotate{everySeconds} | last | blank
      "warm": true,
      "maxSplit": 4 },                        // 1..4: subdivide when several sources are active
    { "id": "t3", "type": "activity", "x": 32, "y": 16, "w": 16, "h": 32,
      "scope": { "kind": "dashboard" },       // | all | list{cameras}
      "direction": "auto", "grouping": true, "objectFocus": true },
    { "id": "t4", "type": "clock", "x": 0, "y": 32, "w": 8, "h": 4, "seconds": true, "date": true }
  ],
  "badges": [ { "id": "b1", "type": "clock", "anchor": "bottom-right", "size": "m" } ],
  "behavior": { "priority": [8, 15, 14, 13], "returnToLiveSeconds": 180, "quietControls": true }
}
```

- A list document has `"items": [...]` instead of `grid`, `target` and `tiles`.
- `resolveDashboard(doc, runtime)` is a pure function that generalises `resolveAutomaticDashboardTiles`:
  - It processes smart tiles in array order.
  - Each smart tile takes the highest-ranked active camera that isn't already taken. If none is active it uses its idle rule, then the next unused source.
  - A camera is never rendered twice.
  - When a camera is borrowed, its home tile shows the displaced camera or a placeholder.

### 2.2 Tables

Follow the `CapturedClipStorage` pattern: add an `EnsureDashboardSchema(sqlite3*)` function and call it next to `EnsureCapturedClipSchema` (`SRV/Database.cpp:1311`).

```sql
CREATE TABLE IF NOT EXISTS Dashboard(
  DashboardUID INTEGER PRIMARY KEY AUTOINCREMENT,
  OwnerUserUID INTEGER NOT NULL,
  Scope INTEGER NOT NULL DEFAULT 0,          -- 0 personal, 1 shared (admin-managed)
  Kind INTEGER NOT NULL DEFAULT 0,           -- 0 canvas, 1 list
  Name TEXT NOT NULL,
  Document TEXT NOT NULL,                    -- canonical JSON re-emitted by the server
  SchemaVersion INTEGER NOT NULL,
  Revision INTEGER NOT NULL DEFAULT 1,
  CreatedAt INTEGER NOT NULL, UpdatedAt INTEGER NOT NULL, UpdatedByUserUID INTEGER NOT NULL,
  FOREIGN KEY(OwnerUserUID) REFERENCES User(UserUID) ON DELETE CASCADE);
CREATE INDEX IF NOT EXISTS DashboardByOwner ON Dashboard(OwnerUserUID, Scope);
CREATE UNIQUE INDEX IF NOT EXISTS DashboardOneListPerUser ON Dashboard(OwnerUserUID) WHERE Kind = 1 AND Scope = 0;

CREATE TABLE IF NOT EXISTS DashboardShare(
  DashboardUID INTEGER NOT NULL,
  PrincipalType INTEGER NOT NULL,            -- 0 everyone, 1 user, 2 group
  PrincipalUID INTEGER NOT NULL,             -- 0 for everyone
  PRIMARY KEY(DashboardUID, PrincipalType, PrincipalUID),
  FOREIGN KEY(DashboardUID) REFERENCES Dashboard(DashboardUID) ON DELETE CASCADE) WITHOUT ROWID;
CREATE INDEX IF NOT EXISTS DashboardShareByPrincipal ON DashboardShare(PrincipalType, PrincipalUID);
```

- Updates use optimistic locking: `UPDATE ... SET Revision = Revision + 1 WHERE DashboardUID = @Id AND Revision = @Base`, then check `sqlite3_changes`.
- Optional later: a `DashboardRevision` table holding the last 10 revisions.

### 2.3 Migration from layouts stored in browsers

The old layouts exist only in browsers, so migration runs in the browser, on the kiosk in particular.

**How the import runs**

1. On first load of the new UI, scan localStorage for `witness-dashboard-layout-v1-*`.
2. Convert each layout with the pure function `migrateLegacyLayout`.
3. `POST /dashboard/create` each one as a personal dashboard named after its group, or "All cameras".
4. Set `witness-dashboard-migrated-v2`. Never delete the old keys.
5. If the user has no dashboards yet, all of this happens without asking.
6. If `fullscreenMode` was true, navigate to `/kiosk/<id>` for the dashboard matching the previously selected group.

**Field mapping**

| v1 | v2 |
|---|---|
| 12×12 rectangles | Multiplied by 4 onto the 48×48 grid (exact) |
| Hidden cameras | Dropped from the canvas, but kept as smart-tile candidates |
| `automaticSlots[i]` | A smart tile at the anchor camera's rectangle. Sources are the priority cameras minus fixed cameras; hold is `slotHoldSeconds`; idle is the fallback camera. |
| Non-custom preset | Generated tiles. A custom arrangement that differs from the default grid is also imported as "<name> – custom". |
| Activity rectangle | An activity tile at the scaled region |
| Activity dock | An edge tile sized from the dock's pixel size on the importing screen. Other tiles are rescaled into the remaining space. |
| `autoFocusEnabled` | "<name>" is built from the focus geometry (a spotlight tile at `focusRegion`, the other cameras at `focusSlots`), plus "<name> – grid" from the regular layout. |
| Clock, return-to-live and quiet-controls settings | A clock badge and the `behavior` settings |

### 2.4 Groups become access-only

- **Schema:** unchanged. Groups still decide which cameras a user can see.
- **Sharing:** groups can also be named as share recipients. Being a recipient grants no camera access.
- **UI removed:**
  - the dashboard group picker
  - "Clips by group" in the sidebar (`AppLayout.vue:102-115, 232-245`)
  - the `/clips/group/:groupId` route
  - `groupId` in `DashboardClipSearch`, replaced by a camera-list filter
- **Server:**
  - `/group/enum` becomes admin-only; today any user gets every group (`SRV/CrowRoutes_Group.cpp:6-45`).
  - `/camera/enum` stops returning group ids to non-admins.
- **Admin page:** renamed "Access groups".

## 3. Server API

The routes go in a new file, `SRV/CrowRoutes_Dashboard.cpp`, registered next to the group routes. Same conventions as today: GET for reads, POST with `csrf` in the body for writes.

### 3.1 Endpoints

| Route | Who can call it | Behaviour |
|---|---|---|
| `GET /dashboard/enum` | any user | Lists visible dashboards: `{id, name, kind, scope, canEdit, revision, updatedAt, tiles, redacted}`. |
| `GET /dashboard/get/<id>` | anyone who can see it | Returns the document, filtered for the viewer. |
| `GET /dashboard/get/<id>?view=edit` | owner, or admin for shared dashboards | Admins get the raw document plus camera names; owners get the filtered document. |
| `POST /dashboard/create` | any user; admin for `scope: shared` | Takes `{name, kind, scope, document}`, returns `{id, revision}`. |
| `POST /dashboard/update` | owner, or admin | Takes `{id, name?, document?, baseRevision}`. Returns 409 with the current revision on a conflict. |
| `POST /dashboard/delete` | owner, or admin | |
| `POST /dashboard/share` | admin | Replaces the recipient list: `[{type: "everyone" \| "user" \| "group", id}]`. |
| `GET /dashboard/access_preview/<id>` | admin | For each recipient, how many tiles would be hidden from them. |

**Status codes**

| Code | Meaning |
|---|---|
| 401 | Not signed in |
| 404 | Missing, or not visible to this user |
| 409 | Revision conflict |
| 413 | Document over 64KB |
| 422 | Invalid: `{error, issues: [{tile, code}]}` |

- **Never return 403.** `api()` redirects to `/login` on 401 or 403 (`UI/composables/useApi.ts:38-43`), which would kick the kiosk off the wall.
- `api()` also needs to raise a typed error that carries the status and the body.
- **"Save a personal copy"** runs in the browser: get the filtered document, then create a new one. A copy can therefore never contain cameras the copier can't see.

### 3.2 Permissions

| | Personal dashboard, owner | Personal dashboard, anyone else (admins included) | Shared dashboard, admin | Shared dashboard, recipient |
|---|---|---|---|---|
| List and view | yes | no (404) | yes | yes, filtered |
| Edit and delete | yes | no | yes | no; can save a personal copy |
| Share | no | no | yes | no |

### 3.3 Validation (pure module `SRV/DashboardDocument.h/.cpp`)

- Check the body size before parsing.
- Parse into structs and re-emit canonical JSON. Unknown fields are dropped and unknown tile types are rejected.
- Limits:

  | Item | Limit |
  |---|---|
  | Tiles per dashboard | 64 |
  | Badges per dashboard | 8 |
  | Sources per smart tile | 64 |
  | Grid size | 4–96 |
  | Name | 80 characters |
  | Dashboards per user | 50 |

- Rectangles must be integers, in bounds and non-overlapping.
- Tile ids must be unique and match `[a-z0-9]{1,12}`.
- Personal dashboards may reference only cameras the author can access. A missing camera and an inaccessible one return the same 422, so the error can't be used to probe which cameras exist.
- Shared dashboards (admin only) may reference any camera that exists.

### 3.4 Filtering what each viewer sees

Filtering runs on every read against the viewer's current access. What is stored is never trusted.

- `LoadAccessibleCameraIds(db, userUID)` wraps `GetCamerasForUser` and keeps only enabled cameras.
- **Camera tile the viewer can't access:** becomes `{"id","type":"unavailable","x","y","w","h","reason":"no-access","redacted":true}`.
  - The camera id, name and label are removed.
  - The rectangle is kept, so the layout doesn't shift.
- **Smart tiles:** inaccessible sources are removed, and an inaccessible idle camera becomes blank. A smart tile with no sources left becomes unavailable.
- **Activity scopes and `behavior.priority`:** inaccessible ids are dropped.
- **Deleted cameras:** treated the same as inaccessible ones.
- **Saving a filtered document:** on update, `MergeRedacted` restores the stored source of any tile sent back with `redacted: true`. A temporary loss of access can't wipe out a reference.
- **Clips:**
  - Add a `cameras=1,2,3` parameter to `/clip/recent` and `/clip/enum`. Values are validated as integers, at most 64.
  - It is intersected with the existing group join and replaces `group=`.
- **Events:**
  - `dashboard:changed {id, revision}`
  - `access:changed`, sent when group mappings change.

### 3.5 Existing access leaks to fix (found by reading the code; not exercised)

1. **`/ws/events`** sends every camera's name, status and motion to every logged-in user (`SRV/CrowListener.cpp:1115-1154`, `SRV/EventBroadcaster.h:54-67, 94-145`).
   - **Fix:** store each connection's user and its set of allowed cameras, tag each broadcast with its camera, and filter.
   - **Call sites to update:** `MotionEvents.cpp:15,90,126,171,211`, `StatusEvents.cpp:30,42`, `MessageDispatcher.cpp:71`, `Recording.cpp:131`, `CrowRoutes_RecentCapture.cpp:104`, `AudioIntelligenceWorker.cpp:245`, `CameraWorker.cpp:341`.
2. **`/clip/calendar` and `/clip/timeline`** are not filtered by user (`SRV/CrowRoutes_Tag.cpp:287-392`).
3. **`/api/detection/<cam>` and `/api/faces/<cam>`** don't check camera access (`SRV/CrowRoutes_Debug.cpp:1441-1450`, `SRV/CrowRoutes_Face.cpp:13-22`).
4. **`/clip/review` (including `all: true`) and `/clip/retag/bulk`** don't check camera access.
5. **Group delete leaves mappings behind** (confirmed).
   - The `DeleteGroup` SQL uses `@Group`, but the handler binds only `@GroupUID` (`SRV/Database.cpp:768-775`, `SRV/CrowRoutes_Group.cpp:155-157`).
   - Group delete must also remove that group's `DashboardShare` rows.

## 4. Front-end architecture

### 4.1 Library choice: our own code, no new runtime dependency

| Option | Version | Assessment |
|---|---|---|
| gridstack | 14.1.1 | Owns the item DOM, compacts items vertically, has no swap or split zones, and can't be tested under `node:test`. |
| grid-layout-plus | 1.1.1 | Uses pixel rows and vertical compaction, needs manual code for dragging in from outside, and pulls in interactjs and vexip. |
| vue-grid-layout | 2.4.0 | Vue 2 only; last released 2022. |
| vue3-grid-layout-next | 1.0.7 | Last released 2024. |

Why write it ourselves:

- The canvas is a fixed grid that doesn't scroll or compact.
- The live renderer must never move a mounted `<video>` to a new parent or reorder it.
- The pointer logic in `CameraGrid.vue:148-199` is already ours.
- A pure geometry module fits the existing transpile-and-`node:test` test style.
- It avoids growing the bundle.

Fallbacks, pinned to exact versions:

- `gridstack@14.1.1`, used in `EditorStage` only.
- `sortablejs@1.15.7`, for the phone reorder list, if our pointer-based reordering proves unreliable on iOS.

### 4.2 Modules (paths under `UI/`)

| File | Role |
|---|---|
| `types/dashboard.ts` | v2 types: `normaliseDoc`, `resolveDashboard`, `migrateLegacyLayout`, `deriveCompactOrder`. No imports. |
| `utils/canvasEngine.ts` | Pure geometry: snap, clamp, move, 8-edge resize, linked resize, swap, split, `findDropTarget`, largest empty rectangle, fill gaps, rescale. Guarantees results are always in bounds and never overlap. |
| `utils/dashboardTemplates.ts` | Templates and re-flow. |
| `stores/dashboards.ts` | List, current dashboard, save with revision, event handlers, legacy import. |
| `stores/dashboardEditor.ts` | Draft, selection, history, dirty flag, target, mode, draft recovery. |
| `composables/useCanvasPointer.ts` | One state machine for all gestures. |
| `composables/useSmartTiles.ts` | Holds, rotation, pinning, freezing during replay. |
| `composables/useKioskChrome.ts` | Idle fade, wake lock, Esc order. |
| `components/dashboard/DashboardCanvas.vue` | Renderer for view, kiosk and preview. |
| `components/dashboard/tiles/*` | `registry.ts`, `CameraTile` (wraps `CameraCard`), `ActivityTile` (wraps `ActivityStrip`), `ClockTile`, `StatusTile`, `UnavailableTile`, `EmptyTile`. |
| `components/dashboard/editor/*` | `DashboardEditor`, `SourcePalette`, `EditorStage`, `TileInspector`, `CanvasInspector`, `TemplateGallery`, `ShareDialog`, `PriorityList`. |
| `components/dashboard/mobile/*` | `MobileSourceView`, `MobileSourcePicker`. |
| `views/DashboardHostView.vue`, `DashboardEditView.vue`, `DashboardManagerView.vue` | Routes. The host view keeps the playback provider, `ClipPlayer`, replay overlay and clip search wiring. |

- **Tile registry:** each tile type registers `{type, label, icon, component, inspector, defaults, minSize}`. A new tile type is one component, one registry entry and one entry in the server whitelist.
- **Reused unchanged:** `CameraCard`, `MsePlayer`, `CameraPlaybackBar`, `DashboardReplayPlayer`, `useDashboardPlayback`, `ActivityStrip`, `StreamIndicator`.
- **Kiosk compatibility:** the kiosk route turns `settings.fullscreenMode` on when entered and off when left.

### 4.3 Renderer invariants (carried over from behaviour the current tests cover)

1. There is one element per mounted camera, keyed by camera id and kept in a stable order.
   - It is positioned only through `grid-area`.
   - When parked it goes off-screen with `inert`.
   - It is never moved in the DOM.
2. The mounted set is the fixed cameras plus the warm smart-tile sources. Resolving the layout changes where each one goes, never which ones are mounted.
3. Unrelated motion never extends a hold timer.
4. Smart tiles don't change during replay.
5. Selecting a tile for control is separate from focus, and Esc deselects first.
6. The live canvas stays a CSS grid: `repeat(var(--cols), minmax(0, 1fr))`.
7. The playback viewport element that `useMseStream` observes stays intact.

## 5. Phased plan

Each phase can ship on its own. The old dashboard stays at `/` until phase 4.

### Phase 0: access hardening (server only; can run in parallel, but must land before phase 7)

- **Work:** fix the section 3.5 leaks and add `LoadAccessibleCameraIds`.
- **Tests:**
  - `EventAccessFilterTests.cpp`
  - `test_access_http.py`, with two users in different groups. Follows the pattern in `TOOLS/Tests/test_recent_capture_http.py`.

### Phase 1: storage and API, no UI (also the "long session" user flag, see Decisions 6)

- **Work:** `DashboardDocument`, `DashboardStorage`, `CrowRoutes_Dashboard.cpp`, the schema hook, and a Vite proxy for `/dashboard`.
- **Tests:**
  - `DashboardDocumentTests.cpp`
  - `DashboardStorageTests.cpp`, covering the visibility matrix, conflicts and cascades
  - `test_dashboard_http.py`, covering redaction, 404, shared dashboards being admin-only, CSRF, 409 and 422

### Phase 2: pure browser-side model

- **Work:** `dashboard.ts`, `canvasEngine.ts` and `dashboardTemplates.ts`.
- **Tests:**
  - `dashboardModel.test.mjs`, porting the existing `dashboardLayout.test.mjs` cases
  - `dashboardMigration.test.mjs`
  - `canvasEngine.test.mjs`, using randomised sequences to show tiles never overlap or leave bounds

### Phase 3: renderer behind a flag (`localStorage['witness-dashboards-v2']`)

- **Work:** the store, `DashboardCanvas`, tile components, `useSmartTiles`, and the `/d/:id` and `/kiosk/:id` routes.
- **Fixture:** add an in-memory `/dashboard/*` API to `TESTS/fixtures/ptzDashboard.mjs`.
- **Tests:**
  - `dashboardCanvas.test.mjs`
  - Playwright `dashboardV2View.test.mjs`:
    - tile boxes at 1366×768, 1920×1080 and 3840×2160
    - redacted tiles make no camera requests and get no 403
    - `dashboard:changed` opens no new stream sockets
    - a kiosk reload restores the view
  - Re-run `verifyDashboardMse.mjs`.

### Phase 4: switching `/` to v2 (revised: no legacy import, see Decisions 1)

- **Work:** the unattended import, the kiosk redirect and the Overview. Keep the old dashboard at `/legacy-dashboard` for one release.
- **Tests:**
  - Playwright `dashboardLegacyImport.test.mjs`
  - Update `dashboardGroupFixture.test.mjs` and `uiPolishFixture.mjs`.

### Phase 5: editor MVP

- **Work:**
  - Palette; move, swap and split; 8-handle and linked resize.
  - Snapping and guides, templates, undo/redo.
  - Inspectors for camera, activity and clock tiles.
  - Save with revision, draft recovery, Preview.
- **Tests:**
  - Playwright `dashboardEditor.test.mjs`
  - `dashboardEditorStore.test.mjs`

### Phase 6: smart tiles in the editor

- **Work:** spotlight and rotation inspectors, the priority list, manual focus and maximise.
- **Tests:** Playwright using `setMotion`.

### Phase 7: sharing

- **Work:** `ShareDialog`, access preview, the manager sections, "Save a personal copy", and a read-only shared view.
- **Tests:**
  - Extend `test_dashboard_http.py`.
  - Playwright `dashboardSharing.test.mjs`, run as a non-admin.

### Phase 8: phone (revised: a per-dashboard camera list with a switcher, see Decisions 7)

- **Work:** `MobileSourceView`, `MobileSourcePicker`, the list document kind, and streaming only cards in view.
- **Tests:**
  - Playwright at 390px and 320px wide.
  - C++ tests for list validation and for one list per user.

### Phase 9: groups become access-only

- **Work:**
  - Remove the group picker, sidebar groups and group route.
  - Make `/group/enum` admin-only.
  - Add the `cameras=` clip filter.
  - Rename the admin page.
- **Tests:** HTTP tests run as a non-admin, and a Playwright sidebar test.

### Phase 10: extras

- **Work:**
  - Revision history and restore.
  - The extra tiles.
  - A `cover` fit option.
  - Delete `DashboardView.vue`.
- `migrateLegacyLayout` and its tests stay.

## 6. Risks and open questions

### Risks

- **Moving the kiosk over.** Today's layout exists only in the kiosk's localStorage. Mitigations: the unattended import, keeping the old keys, and `/legacy-dashboard`.
- **Auto-focus behaves differently** (question 1).
- **Kiosk sessions expire.** The session cookie lasts 2 months and is never renewed (`SRV/CrowRoutes_Auth.cpp:131-132`).
- **Redirect on 403.** If access is revoked mid-session, a request can still send the kiosk to the login page via `api()`.
- **Decoder budget.** Warm smart-tile sources each hold a decoder. Make warm the default only for short source lists.
- **Writing our own editor is work.** Mitigations: the pure geometry engine, its tests, and the gridstack fallback.
- **An existing test is coupled to the old view.** `fullscreenControls.test.mjs` compiles `DashboardView.vue` by path. It must be ported, not deleted.
- **Stale references.** JSON documents can point at deleted cameras; those tiles show as unavailable.
- **`cover` fit is deferred.** The detection overlay assumes `object-fit: contain`.

### Open questions

1. **Auto-focus:** is replacing the whole-canvas re-flow with a spotlight swap acceptable? The alternative is a separate "focus scene" per dashboard.
2. **Admins and cameras:** should admins automatically see every camera? Today they don't.
3. **Admins and personal dashboards:** should admins be unable to see other users' personal dashboards?
4. **Shared dashboards:** should recipients be unable to edit, but able to save a personal copy?
5. **Kiosk setup:**
   - Should the kiosk run as a dedicated user?
   - Should it have a longer-lived session or a device token?
   - Should there be a locked mode with no Exit and no Edit?
6. **Phone list:**
   - Stored per user or per device?
   - Can the activity feed be one of the items?
   - Can an admin set a default list?
7. **Kiosk double-click:** maximise the tile, or exit fullscreen?
8. **Default look:** cards (4px gaps, 8px radius) or seamless (2px)?
9. **Clips navigation:** per camera only, or also "clips for this dashboard"?
10. **First release:** which extra tiles come first, and is revision history included?

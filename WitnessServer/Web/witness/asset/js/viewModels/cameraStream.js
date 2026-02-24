// ── Stream Diagnostics ──────────────────────────────────────────────
// Per-camera event log and stats, retained for up to 24h (reset on page refresh).
// Accessible globally via window._witnessDiag[cameraID] for DevTools use.

var DIAG_MAX_AGE_MS = 24 * 60 * 60 * 1000;

function StreamDiagnostics(cameraID) {
    this.cameraID = cameraID;
    this.startTime = Date.now();
    this.events = [];
    this.stats = {
        restartCount: 0,
        stallCount: 0,
        errorCount: 0,
        maxLatencyMs: 0,
        minLatencyMs: Infinity,
        totalFragments: 0
    };
    this._element = null;
    this._hls = null;
}

StreamDiagnostics.prototype.setRefs = function (element, hls) {
    this._element = element;
    this._hls = hls;
};

StreamDiagnostics.prototype.log = function (type, extra) {
    var now = Date.now();
    var entry = { t: new Date(now).toISOString(), type: type };
    if (extra) {
        for (var k in extra) { if (extra.hasOwnProperty(k)) entry[k] = extra[k]; }
    }
    this.events.push(entry);
    // Prune entries older than 24h
    var cutoff = now - DIAG_MAX_AGE_MS;
    while (this.events.length > 0 && new Date(this.events[0].t).getTime() < cutoff) {
        this.events.shift();
    }
};

StreamDiagnostics.prototype.recordLatency = function (latencyMs) {
    if (latencyMs > this.stats.maxLatencyMs) this.stats.maxLatencyMs = latencyMs;
    if (latencyMs < this.stats.minLatencyMs) this.stats.minLatencyMs = latencyMs;
};

StreamDiagnostics.prototype.snapshot = function () {
    var el = this._element;
    var hls = this._hls;
    var bufferedRanges = [];
    if (el && el.buffered) {
        for (var i = 0; i < el.buffered.length; i++) {
            bufferedRanges.push([el.buffered.start(i), el.buffered.end(i)]);
        }
    }
    return {
        stats: {
            restartCount: this.stats.restartCount,
            stallCount: this.stats.stallCount,
            errorCount: this.stats.errorCount,
            maxLatencyMs: this.stats.maxLatencyMs,
            minLatencyMs: this.stats.minLatencyMs === Infinity ? null : this.stats.minLatencyMs,
            totalFragments: this.stats.totalFragments,
            uptimeMs: Date.now() - this.startTime
        },
        currentState: {
            readyState: el ? el.readyState : null,
            currentTime: el ? el.currentTime : null,
            bufferedRanges: bufferedRanges,
            paused: el ? el.paused : null,
            networkState: el ? el.networkState : null,
            hlsLiveSyncPosition: hls ? hls.liveSyncPosition : null
        },
        events: this.events
    };
};

window._witnessDiag = window._witnessDiag || {};

window._witnessDumpDiag = function () {
    var dump = {
        timestamp: new Date().toISOString(),
        userAgent: navigator.userAgent,
        cameras: {}
    };
    for (var id in window._witnessDiag) {
        if (window._witnessDiag.hasOwnProperty(id)) {
            dump.cameras[id] = window._witnessDiag[id].snapshot();
        }
    }
    var blob = new Blob([JSON.stringify(dump, null, 2)], { type: 'application/json' });
    var url = URL.createObjectURL(blob);
    var a = document.createElement('a');
    a.href = url;
    a.download = 'witness-hls-debug-' + new Date().toISOString().replace(/[:.]/g, '-') + '.json';
    document.body.appendChild(a);
    a.click();
    document.body.removeChild(a);
    URL.revokeObjectURL(url);
};

// ── HLS Binding Helpers ─────────────────────────────────────────────

// Segments arrive every ~2s in bursts of partials. Spinner timeout must
// exceed the segment interval to avoid flickering between deliveries.
var HLS_SPINNER_TIMEOUT_MS = 3000;
var HLS_RESTART_TIMEOUT_MS = 5000;
var HLS_WATCHDOG_INTERVAL_MS = 250;

function createHlsConfig(debug) {
    return {
        debug: !!debug,
        enableWorker: true,
        lowLatencyMode: true,
        liveDurationInfinity: true,
        backBufferLength: 5,
        liveSyncDuration: 2,
        liveMaxLatencyDuration: 5,
        maxLiveSyncPlaybackRate: 1.05
    };
}

function startHlsStream(hls, sourceUrl, element) {
    hls.loadSource(sourceUrl);
    hls.attachMedia(element);
}

// ── hlsPreview binding ──────────────────────────────────────────────

ko.bindingHandlers.hlsPreview = {
    init: function (element, valueAccessor, allBindings, viewModel, bindingContext) {
        var cameraID = ko.unwrap(valueAccessor());
        var sourceUrl = "/stream/" + cameraID;

        var diag = new StreamDiagnostics(cameraID);
        window._witnessDiag[cameraID] = diag;

        var spinner = element.parentNode.querySelector('.CameraActivityIndicator');
        var connLost = element.parentNode.querySelector('.CameraConnectionLost');

        if (spinner) spinner.style.display = 'none';

        if (!Hls.isSupported()) return;

        var hls = new Hls(createHlsConfig(false));
        var lastFragTime = 0;
        var streamStartTime = Date.now();
        var restartInProgress = false;
        var watchdog = null;

        diag.setRefs(element, hls);
        diag.log('start');

        function attachEvents(h) {
            h.on(Hls.Events.FRAG_BUFFERED, function () {
                lastFragTime = Date.now();
                diag.stats.totalFragments++;
                var latencyMs = null;
                if (h.liveSyncPosition != null) {
                    latencyMs = Math.round((h.liveSyncPosition - element.currentTime) * 1000);
                    diag.recordLatency(latencyMs);
                }
                diag.log('frag', { latencyMs: latencyMs });
                viewModel.hlsActive(true);
            });

            h.on(Hls.Events.LEVEL_UPDATED, function () {
                diag.log('levelUpdated');
                viewModel.hlsActive(true);
            });

            h.on(Hls.Events.ERROR, function (event, data) {
                diag.stats.errorCount++;
                diag.log('error', { detail: data.details, fatal: data.fatal, type: data.type });
                if (data.fatal) {
                    viewModel.hlsActive(false);
                    if (data.type === Hls.ErrorTypes.MEDIA_ERROR) {
                        h.recoverMediaError();
                    }
                    // Non-media fatal errors will be caught by the watchdog restart
                }
            });

            h.on(Hls.Events.MEDIA_ATTACHED, function () {
                element.muted = true;
                element.play();
                diag.log('mediaAttached');
                restartInProgress = false;
            });
        }

        function restartStream() {
            if (restartInProgress) return;
            restartInProgress = true;
            diag.stats.restartCount++;
            diag.log('restart', { reason: 'timeout' });
            hls.destroy();
            hls = new Hls(createHlsConfig(false));
            diag.setRefs(element, hls);
            attachEvents(hls);
            lastFragTime = 0;
            streamStartTime = Date.now();
            startHlsStream(hls, sourceUrl, element);
        }

        attachEvents(hls);
        startHlsStream(hls, sourceUrl, element);

        // Poll-based watchdog: drives spinner, connection-lost, and restart.
        // We do NOT seek the playhead — HLS.js has its own recovery for
        // buffer gaps (bufferSeekOverHole, nudgeOnStall). Seeking from
        // outside fights those mechanisms and causes thrashing loops.
        // Instead, if the stream is genuinely stuck, do a full restart.
        var lowReadyStateSince = 0;
        var stuckBackoffMs = 3000;
        watchdog = setInterval(function () {
            // Detect sustained low readyState (frozen frame, playhead
            // stranded outside buffer). Exponential backoff on restarts
            // to avoid churn on streams that can't recover (e.g. pre-
            // recorded content with misaligned timestamps).
            if (!element.paused && element.readyState <= 1 && lastFragTime > 0) {
                if (lowReadyStateSince === 0) {
                    lowReadyStateSince = Date.now();
                } else if (Date.now() - lowReadyStateSince > stuckBackoffMs) {
                    diag.log('stuckRestart', {
                        readyState: element.readyState,
                        currentTime: element.currentTime,
                        liveSyncPosition: hls.liveSyncPosition,
                        backoffMs: stuckBackoffMs
                    });
                    lowReadyStateSince = 0;
                    stuckBackoffMs = Math.min(stuckBackoffMs * 2, 30000);
                    restartStream();
                    return;
                }
            } else {
                lowReadyStateSince = 0;
                // Stream recovered — reset backoff
                if (element.readyState >= 3) stuckBackoffMs = 3000;
            }

            if (lastFragTime === 0) {
                // No fragment received yet — check for initial connection timeout
                var sinceLaunch = Date.now() - streamStartTime;
                if (sinceLaunch > HLS_RESTART_TIMEOUT_MS) {
                    if (connLost) connLost.classList.add('stalled');
                    if (spinner) spinner.style.display = 'none';
                    if (!restartInProgress) {
                        diag.stats.stallCount++;
                        diag.log('initialTimeout', { sinceLaunchMs: sinceLaunch });
                        restartStream();
                    }
                }
                return;
            }
            var elapsed = Date.now() - lastFragTime;

            if (elapsed <= HLS_SPINNER_TIMEOUT_MS) {
                // Data is flowing
                if (spinner) spinner.style.display = '';
                if (connLost) connLost.classList.remove('stalled');
            } else {
                // No recent data
                if (spinner) spinner.style.display = 'none';

                if (elapsed > HLS_RESTART_TIMEOUT_MS) {
                    if (connLost) connLost.classList.add('stalled');
                    if (!restartInProgress) {
                        diag.stats.stallCount++;
                        diag.log('stall', { timeSinceLastFragMs: elapsed });
                        restartStream();
                    }
                }
            }
        }, HLS_WATCHDOG_INTERVAL_MS);

        ko.utils.domNodeDisposal.addDisposeCallback(element, function () {
            clearInterval(watchdog);
            hls.destroy();
        });
    }
};

// ── hlsStream binding ───────────────────────────────────────────────

ko.bindingHandlers.hlsStream = {
    init: function (element, valueAccessor, allBindings, viewModel, bindingContext) {
        var cameraID = ko.unwrap(valueAccessor());
        var sourceUrl = "/stream/" + cameraID;

        var diag = new StreamDiagnostics(cameraID + '_stream');
        window._witnessDiag[cameraID + '_stream'] = diag;

        if (!Hls.isSupported()) return;

        var hls = new Hls(createHlsConfig(true));
        var lastFragTime = 0;
        var streamStartTime = Date.now();
        var restartInProgress = false;
        var watchdog = null;

        diag.setRefs(element, hls);
        diag.log('start');

        function attachEvents(h) {
            h.on(Hls.Events.FRAG_BUFFERED, function () {
                lastFragTime = Date.now();
                diag.stats.totalFragments++;
                var latencyMs = null;
                if (h.liveSyncPosition != null) {
                    latencyMs = Math.round((h.liveSyncPosition - element.currentTime) * 1000);
                    diag.recordLatency(latencyMs);
                }
                diag.log('frag', { latencyMs: latencyMs });
            });

            h.on(Hls.Events.ERROR, function (event, data) {
                diag.stats.errorCount++;
                diag.log('error', { detail: data.details, fatal: data.fatal, type: data.type });
                if (data.fatal) {
                    if (data.type === Hls.ErrorTypes.MEDIA_ERROR) {
                        console.log("fatal media error encountered, try to recover");
                        h.recoverMediaError();
                    }
                }
            });

            h.on(Hls.Events.MEDIA_ATTACHED, function () {
                element.muted = true;
                element.play();
                diag.log('mediaAttached');
                console.log("Video attached and playback started");
                restartInProgress = false;
            });

            h.on(Hls.Events.MANIFEST_PARSED, function (event, data) {
                console.log("Manifest loaded, found " + data.levels.length + " quality level(s)");
            });
        }

        function restartStream() {
            if (restartInProgress) return;
            restartInProgress = true;
            diag.stats.restartCount++;
            diag.log('restart', { reason: 'timeout' });
            console.log("HLS stream stalled, restarting");
            hls.destroy();
            hls = new Hls(createHlsConfig(true));
            diag.setRefs(element, hls);
            attachEvents(hls);
            lastFragTime = 0;
            streamStartTime = Date.now();
            startHlsStream(hls, sourceUrl, element);
        }

        attachEvents(hls);
        startHlsStream(hls, sourceUrl, element);

        var lowReadyStateSince = 0;
        var stuckBackoffMs = 3000;
        watchdog = setInterval(function () {
            if (!element.paused && element.readyState <= 1 && lastFragTime > 0) {
                if (lowReadyStateSince === 0) {
                    lowReadyStateSince = Date.now();
                } else if (Date.now() - lowReadyStateSince > stuckBackoffMs) {
                    diag.log('stuckRestart', {
                        readyState: element.readyState,
                        currentTime: element.currentTime,
                        liveSyncPosition: hls.liveSyncPosition,
                        backoffMs: stuckBackoffMs
                    });
                    lowReadyStateSince = 0;
                    stuckBackoffMs = Math.min(stuckBackoffMs * 2, 30000);
                    restartStream();
                    return;
                }
            } else {
                lowReadyStateSince = 0;
                if (element.readyState >= 3) stuckBackoffMs = 3000;
            }

            if (lastFragTime === 0) {
                var sinceLaunch = Date.now() - streamStartTime;
                if (sinceLaunch > HLS_RESTART_TIMEOUT_MS && !restartInProgress) {
                    diag.stats.stallCount++;
                    diag.log('initialTimeout', { sinceLaunchMs: sinceLaunch });
                    restartStream();
                }
                return;
            }
            var elapsed = Date.now() - lastFragTime;
            if (elapsed > HLS_RESTART_TIMEOUT_MS && !restartInProgress) {
                diag.stats.stallCount++;
                diag.log('stall', { timeSinceLastFragMs: elapsed });
                restartStream();
            }
        }, HLS_WATCHDOG_INTERVAL_MS);

        ko.utils.domNodeDisposal.addDisposeCallback(element, function () {
            clearInterval(watchdog);
            hls.destroy();
        });
    }
};

// ── CameraStreamViewModel ───────────────────────────────────────────

var CameraStreamViewModel = function (parent, camera, cameraID) {
    "use strict";
    var self = this;

    self.parent = parent;
    self.camera = camera;
    self.cameraID = ko.observable(cameraID);
};

ko.bindingHandlers.hlsPreview = {
    init: function (element, valueAccessor, allBindings, viewModel, bindingContext) {
        var cameraID = ko.unwrap(valueAccessor());

        var config = {
            enableWorker: true,
            lowLatencyMode: true,
            liveDurationInfinity: true,
            backBufferLength: 5,
            liveSyncDuration: 2,
            liveMaxLatencyDuration: 5,
            maxLiveSyncPlaybackRate: 1.05
        };

        if (Hls.isSupported()) {
            var hls = new Hls(config);
            var sourceUrl = "/stream/" + cameraID;
            hls.loadSource(sourceUrl);
            hls.attachMedia(element);

            // Activity indicator — find the sibling dot element
            var spinner = element.parentNode.querySelector('.CameraActivityIndicator');
            var connLost = element.parentNode.querySelector('.CameraConnectionLost');
            var connectionLostTimer = null;

            function resetConnectionLostTimer() {
                clearTimeout(connectionLostTimer);
                connectionLostTimer = setTimeout(function () {
                    if (connLost) connLost.classList.add('stalled');
                    if (spinner) spinner.style.display = 'none';
                }, 4000);
            }

            hls.on(Hls.Events.FRAG_BUFFERED, function () {
                if (spinner) spinner.style.display = '';
                if (connLost) {
                    connLost.classList.remove('stalled');
                    resetConnectionLostTimer();
                }
                viewModel.hlsActive(true);
            });

            hls.on(Hls.Events.LEVEL_UPDATED, function () {
                if (connLost) {
                    connLost.classList.remove('stalled');
                    resetConnectionLostTimer();
                }
                viewModel.hlsActive(true);
            });

            hls.on(Hls.Events.ERROR, function (event, data) {
                if (data.details === Hls.ErrorDetails.BUFFER_STALLED_ERROR) {
                    if (connLost) connLost.classList.add('stalled');
                    if (spinner) spinner.style.display = 'none';
                }
                if (data.fatal) {
                    viewModel.hlsActive(false);
                    if (data.type === Hls.ErrorTypes.MEDIA_ERROR) {
                        hls.recoverMediaError();
                    } else {
                        setTimeout(function () {
                            hls.loadSource(sourceUrl);
                            hls.attachMedia(element);
                        }, 5000);
                    }
                }
            });

            hls.on(Hls.Events.MEDIA_ATTACHED, function () {
                element.muted = true;
                element.play();
            });

            ko.utils.domNodeDisposal.addDisposeCallback(element, function () {
                clearTimeout(connectionLostTimer);
                hls.destroy();
            });
        }
    }
};

ko.bindingHandlers.hlsStream = {
    init: function (element, valueAccessor, allBindings, viewModel, bindingContext) {

        var cameraID = ko.unwrap(valueAccessor());

        var config = {
            debug: true,
            enableWorker: true,
            lowLatencyMode: true,
            liveDurationInfinity: true,
            backBufferLength: 5,
            liveSyncDuration: 2,
            liveMaxLatencyDuration: 5,
            maxLiveSyncPlaybackRate: 1.05
        };

        if (Hls.isSupported()) {
            var hls = new Hls(config);

            var sourceUrl = "/stream/" + cameraID;
            hls.loadSource(sourceUrl);
            hls.attachMedia(element);

            hls.on(Hls.Events.ERROR, function (event, data) {
                if (data.fatal) {
                    switch (data.type) {
                        case Hls.ErrorTypes.NETWORK_ERROR:
                            console.log("fatal network error encountered, try to recover");
                            hls.startLoad();
                            break;
                        case Hls.ErrorTypes.MEDIA_ERROR:
                            console.log("fatal media error encountered, try to recover");
                            hls.recoverMediaError();
                            break;
                        default:
                            console.log("fatal error, destroying HLS instance");
                            hls.destroy();
                            break;
                    }
                }
            });

            hls.on(Hls.Events.MEDIA_ATTACHED, function () {
                element.muted = true;
                element.play();
                console.log("Video attached and playback started");
            });

            hls.on(Hls.Events.MANIFEST_PARSED, function (event, data) {
                console.log(
                    "Manifest loaded, found " + data.levels.length + " quality level(s)"
                );
            });
        }
    }
};

var CameraStreamViewModel = function (parent, camera, cameraID) {
    "use strict";
    var self = this;

    self.parent = parent;
    self.camera = camera;
    self.cameraID = ko.observable(cameraID);
};

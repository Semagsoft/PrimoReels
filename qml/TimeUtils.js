.pragma library

// Shared mm:ss:cc (centiseconds) timecode used by the Program Monitor,
// Source Monitor, and Bin duration labels.
function formatTime(seconds) {
    if (!isFinite(seconds) || seconds < 0) {
        seconds = 0;
    }
    var mins = Math.floor(seconds / 60);
    var secs = Math.floor(seconds % 60);
    var cs = Math.floor((seconds % 1) * 100);
    return String(mins).padStart(2, "0") + ":" +
           String(secs).padStart(2, "0") + ":" +
           String(cs).padStart(2, "0");
}

function formatDuration(seconds) {
    if (!isFinite(seconds) || seconds < 0) {
        seconds = 0;
    }
    var mins = Math.floor(seconds / 60);
    var secs = Math.floor(seconds % 60);
    return String(mins).padStart(2, "0") + ":" +
           String(secs).padStart(2, "0");
}

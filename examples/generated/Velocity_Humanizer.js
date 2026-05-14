inlets = 3;
outlets = 3;

var amount = 10;
var minVel = 1;
var maxVel = 127;
var enabled = 1;

function msg_int(v) {
    if (inlet == 1) {
        if (v > 0 && enabled) {
            var offset = Math.round((Math.random() - 0.5) * 2 * amount);
            var newVel = v + offset;
            if (newVel < minVel) newVel = minVel;
            if (newVel > maxVel) newVel = maxVel;
            outlet(1, newVel);
        } else {
            outlet(1, v);
        }
    } else {
        outlet(inlet, v);
    }
}

function msg_float(v) {
    msg_int(Math.round(v));
}

function set_amount(v) {
    amount = Math.max(0, Math.min(64, v));
}

function set_min(v) {
    minVel = Math.max(1, Math.min(127, Math.round(v)));
}

function set_max(v) {
    maxVel = Math.max(1, Math.min(127, Math.round(v)));
}

function set_enabled(v) {
    enabled = v ? 1 : 0;
}

function bang() {
    post("Velocity Humanizer - amount: " + amount + ", range: " + minVel + "-" + maxVel + ", enabled: " + enabled + "\n");
}
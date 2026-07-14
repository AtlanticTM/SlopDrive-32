import { $, setRead, icon, toast } from "../core/ui.js";
import { post } from "../core/api.js";

export let pat = { running: false };

function patPayload() {
    return {
        speed:     parseInt($("patSpeed").value),
        depth:     parseInt($("patDepth").value),
        stroke:    parseInt($("patStroke").value),
        sensation: parseInt($("patSensation").value),
        pattern:   parseInt($("patSelect").value)
    };
}

let pp = null;
export function pushPatParams() {
    clearTimeout(pp);
    pp = setTimeout(function () {
        post("/api/pattern", patPayload());
    }, 60);
}

export function initPattern() {
    // Slider inputs — push params on drag
    ["patSpeed", "patDepth", "patStroke", "patSensation"].forEach(function (id) {
        var s = $(id);
        if (!s) return;
        s.addEventListener("input", function () {
            setRead(id + "Val", s.value);
            pushPatParams();
        });
    });

    // Pattern dropdown — push on change
    var sel = $("patSelect");
    if (sel) {
        sel.addEventListener("change", function () {
            pushPatParams();
        });
    }

    // Start/stop button
    var btn = $("patStartBtn");
    if (btn) btn.addEventListener("click", togglePattern);
}

export async function startPattern() {
    if (pat.running) return;
    pat.running = true;
    await post("/api/pattern", Object.assign(patPayload(), { running: true }));
    var btn = $("patStartBtn");
    if (btn) {
        btn.innerHTML = icon("i-stop") + " Stop Pattern";
        btn.classList.remove("primary");
        btn.classList.add("danger");
    }
    var note = $("patNote");
    if (note) note.textContent = "Pattern running on the device — keeps going even if you close this tab.";
}

export async function stopPattern() {
    if (!pat.running) return;
    pat.running = false;
    await post("/api/pattern", { running: false });
    var btn = $("patStartBtn");
    if (btn) {
        btn.innerHTML = icon("i-play") + " Start Pattern";
        btn.classList.add("primary");
        btn.classList.remove("danger");
    }
    var note = $("patNote");
    if (note) note.textContent = "Patterns run on-device — they keep going even if you close this tab. Stop before switching patterns.";
}

export function togglePattern() {
    pat.running ? stopPattern() : startPattern();
}
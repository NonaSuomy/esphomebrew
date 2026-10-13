#pragma once
// The supplied Scratch SVGs and native toolbar glyphs are embedded so controls
// work without external SD assets. Rasterized once per icon/size by the port.
enum class PaxIcon { Flag, Stop, New, Open, Save, Undo, Redo, Home, Pan, Blocks, Back };
void pax_draw_icon(PaxIcon icon, int x, int y, int size);
static const char *const pax_icon_svg[] = {
R"PAXSVG(<svg id="Layer_1" data-name="Layer 1" xmlns="http://www.w3.org/2000/svg" viewBox="0 0 16.63 17.5"><defs><style>.cls-1,.cls-2{fill:#4cbf56;stroke:#45993d;stroke-linecap:round;stroke-linejoin:round;}.cls-2{stroke-width:1.5px;}</style></defs><title>icon--green-flag</title><path class="cls-1" d="M.75,2A6.44,6.44,0,0,1,8.44,2h0a6.44,6.44,0,0,0,7.69,0V12.4a6.44,6.44,0,0,1-7.69,0h0a6.44,6.44,0,0,0-7.69,0"/><line class="cls-2" x1="0.75" y1="16.75" x2="0.75" y2="0.75"/></svg>)PAXSVG",
R"PAXSVG(<?xml version="1.0" encoding="utf-8"?>
<!-- Generator: Adobe Illustrator 19.1.0, SVG Export Plug-In . SVG Version: 6.00 Build 0)  -->
<svg version="1.1" id="Layer_1" xmlns="http://www.w3.org/2000/svg" xmlns:xlink="http://www.w3.org/1999/xlink" x="0px" y="0px"
	 viewBox="0 0 14 14" style="enable-background:new 0 0 14 14;" xml:space="preserve">
<style type="text/css">
	.st0{fill:#EC5959;stroke:#B84848;stroke-linecap:round;stroke-linejoin:round;stroke-miterlimit:10;}
</style>
<polygon class="st0" points="4.3,0.5 9.7,0.5 13.5,4.3 13.5,9.7 9.7,13.5 4.3,13.5 0.5,9.7 0.5,4.3 "/>
</svg>
)PAXSVG",
R"PAXSVG(<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 24 26"><g fill="none" stroke="#575e75" stroke-width="2" stroke-linecap="round" stroke-linejoin="round"><path d="M5 2h9l5 5v15H5z M14 2v6h5 M8 15h8 M12 11v8"/></g></svg>)PAXSVG",
R"PAXSVG(<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 24 26"><g fill="none" stroke="#575e75" stroke-width="2" stroke-linecap="round" stroke-linejoin="round"><path d="M2 7h8l2 3h10l-3 11H2z M2 7V4h8l2 3h8v3"/></g></svg>)PAXSVG",
R"PAXSVG(<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 24 26"><g fill="none" stroke="#575e75" stroke-width="2" stroke-linecap="round" stroke-linejoin="round"><path d="M3 3h16l2 2v16H3z M7 3v6h10V3 M7 21v-8h10v8"/></g></svg>)PAXSVG",
R"PAXSVG(<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 24 26"><g fill="none" stroke="#575e75" stroke-width="2" stroke-linecap="round" stroke-linejoin="round"><path d="M9 4L3 10l6 5 M3 10h10a7 7 0 0 1 0 14"/></g></svg>)PAXSVG",
R"PAXSVG(<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 24 26"><g fill="none" stroke="#575e75" stroke-width="2" stroke-linecap="round" stroke-linejoin="round"><path d="M15 4l6 6-6 5 M21 10h-10a7 7 0 0 0 0 14"/></g></svg>)PAXSVG",
R"PAXSVG(<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 24 26"><g fill="none" stroke="#575e75" stroke-width="2" stroke-linecap="round" stroke-linejoin="round"><path d="M2 11L12 3l10 8 M5 9v12h5v-7h4v7h5V9"/></g></svg>)PAXSVG",
R"PAXSVG(<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 24 26"><g fill="none" stroke="#575e75" stroke-width="2" stroke-linecap="round" stroke-linejoin="round"><path d="M12 2v20 M2 12h20 M8 6l4-4 4 4 M8 18l4 4 4-4 M6 8l-4 4 4 4 M18 8l4 4-4 4"/></g></svg>)PAXSVG",
R"PAXSVG(<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 24 26"><g fill="none" stroke="#575e75" stroke-width="2" stroke-linecap="round" stroke-linejoin="round"><path d="M3 3h6l2 2h4l2-2h4v7h-4l-2 2h-4l-2-2H3z M3 13h6l2 2h4l2-2h4v7h-4l-2 2h-4l-2-2H3z"/></g></svg>)PAXSVG",
R"PAXSVG(<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 24 26"><g fill="none" stroke="#575e75" stroke-width="2" stroke-linecap="round" stroke-linejoin="round"><path d="M10 4l-8 8 8 8 M2 12h20"/></g></svg>)PAXSVG"
};

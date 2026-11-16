// Dusk's logo and the Settings icon (data/icons/*.svg), built in so they show
// before Dusk is installed too. Keep in step with the files.
#pragma once

inline const char *kDuskLogoSvg = R"svg(<svg xmlns="http://www.w3.org/2000/svg" width="128" height="128" viewBox="0 0 128 128">
  <!-- Dusk: the sun half set on the horizon, two bands of evening through it,
       and the first star out. Plain shapes only (no masks or clipping), so
       every SVG reader draws it the same. -->
  <defs>
    <linearGradient id="sky" x1="0" y1="0" x2="0" y2="1">
      <stop offset="0" stop-color="#1f1a44"/>
      <stop offset="0.6" stop-color="#4b2f6e"/>
      <stop offset="1" stop-color="#b8566b"/>
    </linearGradient>
    <linearGradient id="sun" x1="0" y1="0" x2="0" y2="1">
      <stop offset="0" stop-color="#ffd884"/>
      <stop offset="1" stop-color="#ff7d55"/>
    </linearGradient>
  </defs>
  <circle cx="64" cy="64" r="58" fill="url(#sky)"/>
  <path d="M12.15 90 L115.85 90 A58 58 0 0 1 12.15 90 Z" fill="#2a1f4a"/>
  <rect x="38" y="97" width="52" height="3" rx="1.5" fill="#ff9a66" fill-opacity="0.55"/>
  <rect x="47" y="104.5" width="34" height="3" rx="1.5" fill="#ff9a66" fill-opacity="0.35"/>
  <path d="M39.85 69.00 A32 32 0 0 1 88.15 69.00 Z M33.44 80.50 A32 32 0 0 1 35.73 75.00 L92.27 75.00 A32 32 0 0 1 94.56 80.50 Z M32.00 90.00 A32 32 0 0 1 32.32 85.50 L95.68 85.50 A32 32 0 0 1 96.00 90.00 Z" fill="url(#sun)"/>
  <path d="M88 26 l3.4 9 9 3.4 -9 3.4 -3.4 9 -3.4 -9 -9 -3.4 9 -3.4z" fill="#fff4e0"/>
  <circle cx="64" cy="64" r="57.5" fill="none" stroke="#ffffff" stroke-opacity="0.14"/>
</svg>
)svg";

inline const char *kSettingsIconSvg = R"svg(<svg xmlns="http://www.w3.org/2000/svg" width="128" height="128" viewBox="0 0 128 128">
  <!-- Dusk Settings: the Dusk evening sky, with a cog whose middle is the
       setting sun. Plain shapes only. -->
  <defs>
    <linearGradient id="sky" x1="0" y1="0" x2="0" y2="1">
      <stop offset="0" stop-color="#1f1a44"/>
      <stop offset="0.6" stop-color="#4b2f6e"/>
      <stop offset="1" stop-color="#b8566b"/>
    </linearGradient>
    <linearGradient id="sun" x1="0" y1="0" x2="0" y2="1">
      <stop offset="0" stop-color="#ffd884"/>
      <stop offset="1" stop-color="#ff7d55"/>
    </linearGradient>
  </defs>
  <circle cx="64" cy="64" r="58" fill="url(#sky)"/>
  <g transform="translate(64 64)" fill="#f4eefc"><rect x="-7" y="-40" width="14" height="15" rx="3.5" transform="rotate(0)"/><rect x="-7" y="-40" width="14" height="15" rx="3.5" transform="rotate(45)"/><rect x="-7" y="-40" width="14" height="15" rx="3.5" transform="rotate(90)"/><rect x="-7" y="-40" width="14" height="15" rx="3.5" transform="rotate(135)"/><rect x="-7" y="-40" width="14" height="15" rx="3.5" transform="rotate(180)"/><rect x="-7" y="-40" width="14" height="15" rx="3.5" transform="rotate(225)"/><rect x="-7" y="-40" width="14" height="15" rx="3.5" transform="rotate(270)"/><rect x="-7" y="-40" width="14" height="15" rx="3.5" transform="rotate(315)"/><circle r="28"/></g>
  <circle cx="64" cy="64" r="18" fill="#2a1f4a"/>
  <path d="M52.74 63.50 A13 13 0 0 1 75.26 63.50 Z M51.00 70.00 A13 13 0 0 1 51.63 66.00 L76.37 66.00 A13 13 0 0 1 77.00 70.00 Z" fill="url(#sun)"/>
  <rect x="51" y="72.5" width="26" height="2.5" rx="1.25" fill="#ff9a66" fill-opacity="0.6"/>
  <circle cx="64" cy="64" r="57.5" fill="none" stroke="#ffffff" stroke-opacity="0.14"/>
</svg>
)svg";

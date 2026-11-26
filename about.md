# <cg>RenderLite</c>

<cy>A performance toolkit for Geometry Dash.</c>

Get more out of your PC while you play. Every feature can be turned on or off, and most have an adjustable strength. The strongest option is always named in the setting's description.

Source and updates: [github.com/mikhailrasanen1304-dot/RenderLite](https://github.com/mikhailrasanen1304-dot/RenderLite)

## <cg>Draw divide</c>

Draws the picture at a lower frame rate than the game logic. The game still runs physics and reads your clicks at the full FPS, but the graphics card only draws the screen at the Render FPS. That means a lighter load for your GPU and the low input delay of a high FPS. The idea comes from the Draw Divide mod by matcool.

- **Draw divide:** turn it on or off. <cy>It only has an effect when the game runs at a higher FPS than the Render FPS.</c>
- **Draw divide hotkey:** press a key while playing to switch Draw divide on or off, with a small message on screen. <cg>Default: F8.</c> Pick None to turn it off.
- **Render FPS:** 0 matches your monitor's refresh rate. <cg>Strongest saving: a low number such as 30.</c>
- **Game logic FPS:** how fast physics and input run. <cg>Strongest: 200.</c> 0 leaves the game's own setting alone.

## <cg>System settings</c>

- **Process priority:** gives the game more CPU time. <cg>Strongest: High.</c>
- **Power throttling:** stops Windows from slowing the game down to save power. <cg>Strongest: CPU and timer throttling off.</c>
- **Timer resolution:** more precise frame timing. <cg>Strongest: 1 ms.</c>
- **Power plan:** switch between Balanced, High performance and Ultimate performance while you play. <cg>Strongest: Ultimate performance.</c> Your old plan comes back when the game closes.
- **Background apps:** lowers the priority of your other programs so the game gets most of the CPU. <cg>Strongest: Strongest.</c> They are restored when the game closes.
- **Focus mode:** hides Windows notification pop-ups while you play.
- **GPU preference:** picks which graphics card the game uses. <cg>Strongest: High performance GPU.</c> <cy>Restart required.</c>

## <cg>Other settings</c>

- **Fast loading:** speeds up the Geometry Dash loading screen. <cy>Restart required.</c>
- **Anti-crash (experimental):** tries to skip a broken frame instead of closing the game. Off by default.

## <cy>Good to know</c>

- Draw divide works best with the FPS unlocked. Turn on Unlock FPS in the game's graphics settings, or use the Game logic FPS setting.
- The hotkey only works while the game window is in front. If another mod already uses the same key, pick a different one.
- These options do not raise your FPS directly. They help most when other programs are using your CPU or when Windows is limiting the game.
- Anti-crash cannot stop every crash, and the game may act oddly after an error.
- Fast loading works best with V-Sync off. It cannot speed up Geode's own loading.
- Focus mode only hides pop-ups. Windows does not let mods switch its Focus assist on.
- Power plan, Focus mode and Background apps are put back by a small hidden PowerShell helper after the game closes, even after a crash. Some antivirus programs may ask about it. If you do not want that, keep those three settings off.
- Background apps never touches system programs, Discord, OBS or Steam.

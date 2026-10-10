# 2026-10-10 home PC, real headset (Quest 3 via Virtual Desktop, sensor covered, nobody wearing it)

- Continue (virtual pad) -> Ch.1 opening car cutscene. OpenXR on the headset thread's own device: session running,
  both eye images shared, head-tracked views fov L-40 R40 U18.6 D-18.6 `[verified-live 2026-10-10, n=1]`.
- Headset frames: 900 per 12.5 s = **72/s**. Game Presents: a steady **~43.7/s** in the cutscene, and with AFR each
  eye gets a new picture only every other Present, so **~22 new pictures per eye per second** `[measured 2026-10-10]`.
  That is likely to look stepped in the headset; whether the cap is the cutscene (the game's 30 fps cutscene lock?)
  or the AFR path is not known `[hypothesis]`. Measure in gameplay next time.
- Not reached: gameplay (the cutscene has no quit/skip in its pause screen). Closed with WM_CLOSE, clean exit.
- Video (flat window): MEGA Videos/the-evil-within-vr/headset first look_2026-10-10_21-42-33.mp4.

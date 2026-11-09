# `react-native-reanimated` patches

### [react-native-reanimated+4.7.0+001+preserve-css-animations-during-exiting.patch](react-native-reanimated+4.7.0+001+preserve-css-animations-during-exiting.patch)

- Reason:

    ```
    On web, an `exiting` layout animation moves the children of the exiting view into a copy of the view. Moving an element restarts its CSS animations, so the children's running CSS animations restarted from their first keyframe, finished animations replayed, and a child's animation ending ended the exiting animation early.

    This patch keeps the children's CSS animations going through the move, and ignores animation events from descendants. LoadingBar needs it to fade out with an `exiting` animation while its CSS-animated indicator keeps moving.
    ```

- Upstream PR/issue: https://github.com/software-mansion/react-native-reanimated/pull/11091, https://github.com/software-mansion/react-native-reanimated/issues/11089
- E/App issue: 🛑
- PR introducing patch: https://github.com/Expensify/App/pull/105098

### [react-native-reanimated+4.7.0+002+start-exiting-from-current-values.patch](react-native-reanimated+4.7.0+002+start-exiting-from-current-values.patch)

- Reason:

    ```
    When a view unmounts while its `entering` animation is still running, its `exiting` animation starts from its initial values. A view fading in with `FadeIn` jumped to full opacity before `FadeOut` faded it out, on iOS, Android and web.

    This patch starts the properties that the interrupted animation drives from their current values: all of them on iOS and Android, and `opacity` on web. LoadingBar needs it to fade in and out on one view, because a bar hidden during its fade-in would otherwise flash to full opacity first.
    ```

- Upstream PR/issue: https://github.com/software-mansion/react-native-reanimated/pull/11093, https://github.com/software-mansion/react-native-reanimated/issues/11092
- E/App issue: 🛑
- PR introducing patch: https://github.com/Expensify/App/pull/105098

### [react-native-reanimated+4.7.0+003+preserve-native-css-animations-during-exiting.patch](react-native-reanimated+4.7.0+003+preserve-native-css-animations-during-exiting.patch)

- Reason:

    ```
    On native, React removes a LoadingBar and its indicator from the shadow tree when loading ends, while the parent's exiting layout animation keeps their native views mounted. The mount hook removes the indicator's CSS animation from the updates registry. Skipping that cleanup alone does not fix the freeze: normal CSS updates clone the live shadow tree, which no longer contains the indicator.

    This patch keeps animation registries alive until the layout animation proxy releases the native view. It retains the last mounted shadow tree privately, applies subsequent animation updates to that snapshot, and runs Yoga so percentage lengths and descendant layouts keep updating. Only updates to views still retained by an exit are sent to the mounting layer; the snapshot never becomes React's live tree. Both native layout animation proxies accept these updates. Completed exits, remounts and surface removal release the retained snapshots and animation registrations.
    ```

- Upstream PR/issue: Not submitted. The native reproduction is reported in https://github.com/Expensify/App/pull/105098#issuecomment-6628023865; a separate upstream fix needs device verification.
- E/App issue: https://github.com/Expensify/App/issues/104799
- PR introducing patch: https://github.com/Expensify/App/pull/105098

- Regression check: On macOS, with Xcode command line tools and the `folly`, `fmt` and `glog` development libraries available, run `python3 patches/react-native-reanimated/tests/run.py --fast-float-include /path/to/fast_float/include`. The include directory must contain the headers for React Native's pinned `fast_float` v8.0.0. The runner defaults to `/opt/homebrew` for the other native dependencies; use `--dependency-prefix` to change it. It compiles the installed native sources against real React Native shadow nodes, Yoga and JavaScriptCore, and checks retained animation lifetime, percentage layout, mounting updates, opacity preservation, queued frames, relative values, remounts and cleanup on multiple surfaces. It does not replace device verification of LoadingBar's fade-out.

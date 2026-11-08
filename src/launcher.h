/* Double-click start: the command line run.py would pass, built from paths.ini. */
#ifndef DECEPTION_LAUNCH_H
#define DECEPTION_LAUNCH_H

/* deception started with no boot file (no arguments, or only options): reads paths.ini next to
 * the exe (deception-setup writes it), checks game/ was prepared for this release (release.txt)
 * and replaces *argc and *argv with
 * <exe> game/boot-linked.bin --content-profile game/content.bin --app0 <game> --user <save>
 * --timeout 0 [--isa-rewrite game/isa-rewrite.bin] and then the options given. Started without a
 * console or terminal (Explorer, a file manager, a shortcut), output goes to
 * <save>/log/deception.log; --console keeps it on (Windows: opens) a console instead. 0 on
 * success; otherwise it has told the user why. */
int launch_from_paths_ini(int *argc, char ***argv);

#endif

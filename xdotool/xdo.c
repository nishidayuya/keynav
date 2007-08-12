/* xdo library
 *
 * $Id$
 *
 * - getwindowfocus contributed by Lee Pumphret
 * - keysequence_{up,down} contributed by Magnus Boman
 *
 */

#include <sys/select.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <regex.h>

#include <X11/Xlib.h>
#include <X11/Xresource.h>
#include <X11/Xutil.h>
#include <X11/extensions/XTest.h>

#include "xdo.h"
#include "xdo_util.h"

static void _xdo_populate_charcode_map(xdo_t *xdo);
static int _xdo_has_xtest(xdo_t *xdo);

static int _xdo_keycode_from_char(xdo_t *xdo, char key);
static int _xdo_get_shiftcode_if_needed(xdo_t *xdo, char key);

static void _xdo_get_child_windows(xdo_t *xdo, Window window,
                                   Window **total_window_list, 
                                   int *ntotal_windows, 
                                   int *window_list_size);

static int _xdo_keysequence_to_keycode_list(xdo_t *xdo, char *keyseq, int **keys, int *nkeys);
static int _xdo_keysequence_do(xdo_t *xdo, char *keyseq, int pressed);
static int _xdo_regex_match_window(xdo_t *xdo, Window window, int flags, regex_t *re);
static int _xdo_is_window_visible(xdo_t *xdo, Window wid);

static int _is_success(const char *funcname, int code);

/* context-free functions */
char _keysym_to_char(char *keysym);

xdo_t* xdo_new(char *display_name) {
  Display *xdpy;

  if ((xdpy = XOpenDisplay(display_name)) == NULL) {
    fprintf(stderr, "Error: Can't open display: %s\n", display_name);
    return NULL;
  }

  return xdo_new_with_opened_display(xdpy, display_name, 1);
}

xdo_t* xdo_new_with_opened_display(Display *xdpy, char *display, 
                                   int close_display_when_freed) {
  xdo_t *xdo = NULL;

  if (xdpy == NULL) {
    fprintf(stderr, "xdo_new: xdisplay I was given is a null pointer\n");
    return NULL;
  }

  /* XXX: Check for NULL here */
  xdo = malloc(sizeof(xdo_t));
  memset(xdo, 0, sizeof(xdo_t));

  xdo->xdpy = xdpy;
  xdo->close_display_when_freed = close_display_when_freed;

  if (display == NULL)
    display = "unknown";

  if (!_xdo_has_xtest(xdo)) {
    fprintf(stderr, "Error: XTEST extension unavailable on '%s'.", 
            xdo->display_name);
    xdo_free(xdo);
    return NULL;
  }

  /* populate the character map? */
  _xdo_populate_charcode_map(xdo);

  return xdo;
}

void xdo_free(xdo_t *xdo) {
  if (xdo->display_name)
    free(xdo->display_name);
  if (xdo->charcodes)
    free(xdo->charcodes);
  if (xdo->xdpy && xdo->close_display_when_freed)
    XCloseDisplay(xdo->xdpy);
  free(xdo);
}

int xdo_window_map(xdo_t *xdo, int wid) {
  int ret;
  ret = XMapWindow(xdo->xdpy, wid);
  XFlush(xdo->xdpy);
  return _is_success("XMapWindow", ret);
}

int xdo_window_unmap(xdo_t *xdo, int wid) {
  int ret;
  ret = XUnmapWindow(xdo->xdpy, wid);
  XFlush(xdo->xdpy);
  return _is_success("XMapWindow", ret);
}

void xdo_window_list_by_regex(xdo_t *xdo, char *regex, int flags,
                              Window **windowlist, int *nwindows) {
  regex_t re;
  Window *total_window_list = NULL;
  int ntotal_windows = 0;
  int window_list_size = 0;
  int matched_window_list_size = 100;

  int ret;

  ret = regcomp(&re, regex, REG_EXTENDED | REG_ICASE);
  if (ret != 0) {
    fprintf(stderr, "Failed to compile regex: '%s'\n", regex);
    return;
  }

  /* Default search settings:
   * All windows (visible and hidden) and search all text pieces
   */
  if ((flags & (SEARCH_TITLE | SEARCH_CLASS | SEARCH_NAME)) == 0) {
    fprintf(stderr, "No text fields specified for regex search. \nDefaulting to"
            " window title, class, and name searching\n");
    flags = SEARCH_TITLE | SEARCH_CLASS | SEARCH_NAME;
  }

  *nwindows = 0;
  *windowlist = malloc(matched_window_list_size * sizeof(Window));

  _xdo_get_child_windows(xdo, XDefaultRootWindow(xdo->xdpy), 
                         &total_window_list, &ntotal_windows,
                         &window_list_size);
  int i;
  for (i = 0; i < ntotal_windows; i++) {
    Window wid = total_window_list[i];
    if (flags & SEARCH_VISIBLEONLY && !_xdo_is_window_visible(xdo, wid))
      continue;

    if (!_xdo_regex_match_window(xdo, wid, flags, &re))
      continue;

    (*windowlist)[*nwindows] = wid;
    (*nwindows)++;

    if (matched_window_list_size == *nwindows) {
      matched_window_list_size *= 2;
      *windowlist = realloc(*windowlist, 
                            matched_window_list_size * sizeof(Window));
    }
  }

  regfree(&re);
}

int xdo_window_move(xdo_t *xdo, int wid, int x, int y) {
  XWindowChanges wc;
  int ret;
  wc.x = x;
  wc.y = y;

  ret = XConfigureWindow(xdo->xdpy, wid, CWX | CWY, &wc);
  return _is_success("XConfigureWindow", ret);
}

int xdo_window_setsize(xdo_t *xdo, int wid, int width, int height, int flags) {
  XWindowChanges wc;
  int ret;
  int cw_flags = 0;

  wc.width = width;
  wc.height = height;

  if (flags & SIZE_USEHINTS) {
    XSizeHints hints;
    long supplied_return;
    memset(&hints, 0, sizeof(hints));
    XGetWMNormalHints(xdo->xdpy, wid, &hints, &supplied_return);
    if (supplied_return & PResizeInc) {
      wc.width *= hints.width_inc;
      wc.height *= hints.height_inc;
    } else {
      fprintf(stderr, "No size hints found for this window\n");
    }

    if (supplied_return & PBaseSize) {
      wc.width += hints.base_width;
      wc.height += hints.base_height;
    }

  }

  if (width > 0)
    cw_flags |= CWWidth;
  if (height > 0)
    cw_flags |= CWHeight;

  ret = XConfigureWindow(xdo->xdpy, wid, cw_flags, &wc);
  XFlush(xdo->xdpy);
  return _is_success("XConfigureWindow", ret);
}

int xdo_window_focus(xdo_t *xdo, int wid) {
  int ret;
  ret = XSetInputFocus(xdo->xdpy, wid, RevertToParent, CurrentTime);
  XFlush(xdo->xdpy);
  return _is_success("XSetInputFocus", ret);
}

/* XRaiseWindow is ignored in ion3 and Gnome2. Is it even useful? */
int xdo_window_raise(xdo_t *xdo, int wid) {
  int ret;
  ret = XRaiseWindow(xdo->xdpy, wid);
  XFlush(xdo->xdpy);
  return _is_success("XRaiseWindow", ret);
}

/* XXX: Include 'screen number' support? */
int xdo_mousemove(xdo_t *xdo, int x, int y)  {
  int ret;
  ret = XTestFakeMotionEvent(xdo->xdpy, -1, x, y, CurrentTime);
  XFlush(xdo->xdpy);
  return _is_success("XTestFakeMotionEvent", ret);
}

int xdo_mousedown(xdo_t *xdo, int button) {
  int ret;
  ret = XTestFakeButtonEvent(xdo->xdpy, button, True, CurrentTime);
  XFlush(xdo->xdpy);
  return _is_success("XTestFakeButtonEvent", ret);
}

int xdo_mouseup(xdo_t *xdo, int button) {
  int ret;
  ret = XTestFakeButtonEvent(xdo->xdpy, button, False, CurrentTime);
  XFlush(xdo->xdpy);
  return _is_success("XTestFakeKeyEvent", ret);
}

int xdo_click(xdo_t *xdo, int button) {
  int ret;
  ret = xdo_mousedown(xdo, button);
  if (!ret)
    return ret;
  ret = xdo_mouseup(xdo, button);
  return ret;

  /* no need to flush here */
}

/* XXX: Return proper code if errors found */
int xdo_type(xdo_t *xdo, char *string) {
  int i = 0;
  char key = '0';
  int keycode = 0;
  int shiftcode = 0;

  for (i = 0; string[i] != '\0'; i++) {
    key = string[i];
    keycode = _xdo_keycode_from_char(xdo, key);
    shiftcode = _xdo_get_shiftcode_if_needed(xdo, key);

    if (shiftcode)
      XTestFakeKeyEvent(xdo->xdpy, shiftcode, True, CurrentTime);
    XTestFakeKeyEvent(xdo->xdpy, keycode, True, CurrentTime);
    XTestFakeKeyEvent(xdo->xdpy, keycode, False, CurrentTime);
    if (shiftcode)
      XTestFakeKeyEvent(xdo->xdpy, shiftcode, False, CurrentTime);

    /* XXX: Flush here or at the end? */
    XFlush(xdo->xdpy);
  }

  return True;
}

int _xdo_keysequence_do(xdo_t *xdo, char *keyseq, int pressed) {
  int *keys = NULL;
  int nkeys;

  if (_xdo_keysequence_to_keycode_list(xdo, keyseq, &keys, &nkeys) == False) {
    fprintf(stderr, "Failure converting key sequence '%s' to keycodes\n", keyseq);
    return False;
  }

  int i;
  for (i = 0; i < nkeys; i++) {
    //fprintf(stderr, "Typing %d (%d)\n", keys[i], pressed);
    XTestFakeKeyEvent(xdo->xdpy, keys[i], pressed, CurrentTime);
  }

  free(keys);
  XFlush(xdo->xdpy);
  return True;
}
  
int xdo_keysequence_down(xdo_t *xdo, char *keyseq) {
  return _xdo_keysequence_do(xdo, keyseq, True);
}

int xdo_keysequence_up(xdo_t *xdo, char *keyseq) {
  return _xdo_keysequence_do(xdo, keyseq, False);
}

int xdo_keysequence(xdo_t *xdo, char *keyseq) {
  _xdo_keysequence_do(xdo, keyseq, True);
  _xdo_keysequence_do(xdo, keyseq, False);
  return True;
}


/* Add by Lee Pumphret 2007-07-28
 * Modified slightly by Jordan Sissel */
int xdo_window_get_focus(xdo_t *xdo, int *window_ret) {
  int ret;
  int unused_revert_ret;
  ret = XGetInputFocus(xdo->xdpy, (Window*)window_ret, &unused_revert_ret);
  return _is_success("XGetInputFocus", ret);
}


/* Helper functions */
static int _xdo_keycode_from_char(xdo_t *xdo, char key) {
  int i = 0;
  int len = xdo->keycode_high - xdo->keycode_low;

  for (i = 0; i < len; i++)
    if (xdo->charcodes[i].key == key)
      return xdo->charcodes[i].code;

  return -1;
}

static int _xdo_get_shiftcode_if_needed(xdo_t *xdo, char key) {
  int i = 0;
  int len = xdo->keycode_high - xdo->keycode_low;

  for (i = 0; i < len; i++)
    if (xdo->charcodes[i].key == key)
      return xdo->charcodes[i].shift;

  return -1;
}

static int _xdo_has_xtest(xdo_t *xdo) {
  int dummy;
  return (XTestQueryExtension(xdo->xdpy, &dummy, &dummy, &dummy, &dummy) == True);
}

static void _xdo_populate_charcode_map(xdo_t *xdo) {
  /* assert xdo->display is valid */
  int keycodes_length = 0;
  int shift_keycode = 0;
  int i, j;

  XDisplayKeycodes(xdo->xdpy, &(xdo->keycode_low), &(xdo->keycode_high));

  /* Double size of keycode range because some 
   * keys have "shift" values. ie; 'a' and 'A', '2' and '@' */
  /* Add 2 to the size because the range [low, high] is inclusive */
  keycodes_length = (xdo->keycode_high - xdo->keycode_low) * 2 + 2;
  xdo->charcodes = malloc(keycodes_length * sizeof(charcodemap_t));
  memset(xdo->charcodes, 0, keycodes_length * sizeof(charcodemap_t));

  /* Fetch the keycode for Shift_L */
  /* XXX: Make 'Shift_L' configurable? */
  shift_keycode = XKeysymToKeycode(xdo->xdpy, XStringToKeysym("Shift_L"));

  for (i = xdo->keycode_low; i <= xdo->keycode_high; i++) {
    char *keybuf = 0;

    /* Index '0' in KeycodeToKeysym == no shift key
     * Index '1' in ... == shift key held
     * hence this little loop. */
    for (j = 0; j <= 1; j++) { 
     int index = (i - xdo->keycode_low) * 2 + j;
     keybuf = XKeysymToString(XKeycodeToKeysym(xdo->xdpy, i, j));

     xdo->charcodes[index].key = _keysym_to_char(keybuf);
     xdo->charcodes[index].code = i;
     xdo->charcodes[index].shift = j ? shift_keycode : 0;
    }
  }
}

/* context-free functions */
char _keysym_to_char(char *keysym) {
  int i;

  if (keysym == NULL)
    return -1;

  /* keysymcharmap comes from xdo_util.h */
  for (i = 0; keysymcharmap[i].keysym; i++) {
    if (!strcmp(keysymcharmap[i].keysym, keysym))
      return keysymcharmap[i].key;
  }

  if (strlen(keysym) == 1)
    return keysym[0];

  return -1;
}

  /* regexec(&re, string, 0, NULL, 0) == 0 means MATCH */

static void _xdo_get_child_windows(xdo_t *xdo, Window window,
                                   Window **total_window_list, 
                                   int *ntotal_windows,
                                   int *window_list_size) {
  Window dummy;
  int i;
  Window *children;
  unsigned int nchildren;

  if (*window_list_size == 0) {
    *ntotal_windows = 0;
    *window_list_size = 100;
    *total_window_list = malloc(*window_list_size * sizeof(Window));
  }

  /* foo */
  if (!XQueryTree(xdo->xdpy, window, &dummy, &dummy, &children, &nchildren))
    return;

  for (i = 0; i < nchildren; i++) {
    Window w = children[i];
    (*total_window_list)[*ntotal_windows] = w;
    *ntotal_windows += 1;
    if (*ntotal_windows == *window_list_size) {
      *window_list_size *= 2;
      *total_window_list = realloc(*total_window_list,
                                   *window_list_size * sizeof(Window));
    }
    _xdo_get_child_windows(xdo, w, total_window_list,
                           ntotal_windows, window_list_size);
  }

  XFree(children);
}

int _xdo_keysequence_to_keycode_list(xdo_t *xdo, char *keyseq, int **keys, int *nkeys) {
  char *tokctx = NULL;
  char *tok = NULL;
  char *strptr = NULL;
  int i;
  
  /* Array of keys to press, in order given by keyseq */
  int keys_size = 10;
  *nkeys = 0;

  if (strcspn(keyseq, " \t\n.-[]{}\\|") != strlen(keyseq)) {
    fprintf(stderr, "Error: Invalid key sequence '%s'\n", keyseq);
    return False;
  }

  *keys = malloc(keys_size * sizeof(int));
  strptr = strdup(keyseq);
  while ((tok = strtok_r(strptr, "+", &tokctx)) != NULL) {
    int keysym;
    if (strptr != NULL)
      strptr = NULL;

    /* Check if 'tok' (string keysym) is an alias to another key */
    /* symbol_map comes from xdo.util */
    for (i = 0; symbol_map[i] != NULL; i+=2)
      if (!strcasecmp(tok, symbol_map[i]))
        tok = symbol_map[i + 1];

    keysym = XStringToKeysym(tok);
    if (keysym == NoSymbol) {
      fprintf(stderr, "(symbol) No such key name '%s'. Ignoring it.\n", tok);
      continue;
    }

    (*keys)[*nkeys] = XKeysymToKeycode(xdo->xdpy, keysym);

    if ((*keys)[*nkeys] == 0) {
      fprintf(stderr, "No such key '%s'. Ignoring it.\n", tok);
      continue;
    }

    (*nkeys)++;
    if (*nkeys == keys_size) {
      keys_size *= 2;
      *keys = realloc(*keys, keys_size);
    }
  }

  free(strptr);

  return True;
}

int _xdo_regex_match_window(xdo_t *xdo, Window window, int flags, regex_t *re) {
  XWindowAttributes attr;
  XTextProperty tp;
  XClassHint classhint;
  int i;

  XGetWindowAttributes(xdo->xdpy, window, &attr);

  /* XXX: Memory leak here according to valgrind? */
  XGetWMName(xdo->xdpy, window, &tp);

  if (flags & SEARCH_TITLE) {
    if (tp.nitems > 0) {
      int count = 0;
      char **list = NULL;
      XmbTextPropertyToTextList(xdo->xdpy, &tp, &list, &count);
      for (i = 0; i < count; i++) {
        if (regexec(re, list[i], 0, NULL, 0) == 0) {
          XFreeStringList(list);
          return True;
        }
        XFreeStringList(list);
      }
    }
  }

  if (XGetClassHint(xdo->xdpy, window, &classhint)) {
    if ((flags & SEARCH_NAME) && classhint.res_name) {
      if (regexec(re, classhint.res_name, 0, NULL, 0) == 0) {
        XFree(classhint.res_name);
        XFree(classhint.res_class);
        return True;
      }
      XFree(classhint.res_name);
    }
    if ((flags & SEARCH_CLASS) && classhint.res_class) {
      if (regexec(re, classhint.res_class, 0, NULL, 0) == 0) {
        XFree(classhint.res_class);
        return True;
      }
      XFree(classhint.res_class);
    }
  }
  return False;
}

int _is_success(const char *funcname, int code) {
  if (code == BadMatch) {
    fprintf(stderr, "%s failed: got bad match\n", funcname);
    return False;
  } else if (code == BadValue) {
    fprintf(stderr, "%s failed: got bad value\n", funcname);
    return False;
  } else if (code == BadWindow) {
    fprintf(stderr, "%s failed: got bad window\n", funcname);
    return False;
  }

  return True;
}

int _xdo_is_window_visible(xdo_t *xdo, Window wid) {
  XWindowAttributes wattr;

  XGetWindowAttributes(xdo->xdpy, wid, &wattr);
  if (wattr.map_state != IsViewable)
    return False;

  return True;
}

/* main test */
#ifdef BUILDMAIN
int main(int argc, char **argv) {
  char *display_name;
  xdo_t *xdo;

  char *yay;

  if ( (display_name = getenv("DISPLAY")) == (void *)NULL) {
    fprintf(stderr, "Error: DISPLAY environment variable not set\n");
    exit(1);
  }

  //yay = strdup("ctrl+l");

  xdo = xdo_new(display_name);
  //xdo_mousemove(xdo, 100, 100);
  //usleep(100 * 1000);
  //xdo_keysequence(xdo, strdup("ctrl+l"));
  //xdo_type(xdo, strdup("ls"));
  //xdo_keysequence(xdo, strdup("Return"));

  
  Window *list;
  int nwindows;
  char *query = "xterm";
  int i;
  if (argc > 1)
    query = argv[1];

  xdo_window_list_by_regex(xdo, query, &list, &nwindows);
  for (i = 0; i < nwindows; i++) {
    printf("%d\n", list[i]);
  }
  xdo_free(xdo);

  return 0;
}
#endif


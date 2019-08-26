#include "vterm_internal.h"

#include <stdio.h>
#include <string.h>

#include "rect.h"
#include "utf8.h"

#define UNICODE_SPACE 0x20
#define UNICODE_LINEFEED 0x0a

/* State of the pen at some moment in time, also used in a cell */
typedef struct
{
  /* After the bitfield */
  VTermColor   fg, bg;

  unsigned int bold      : 1;
  unsigned int underline : 2;
  unsigned int italic    : 1;
  unsigned int blink     : 1;
  unsigned int reverse   : 1;
  unsigned int strike    : 1;
  unsigned int font      : 4; /* 0 to 9 */

  /* Extra state storage that isn't strictly pen-related */
  unsigned int protected_cell : 1;
  unsigned int dwl            : 1; /* on a DECDWL or DECDHL line */
  unsigned int dhl            : 2; /* on a DECDHL line (1=top 2=bottom) */
  unsigned int wraparound     : 1; /* This row is a continuation of the last line */
} ScreenPen;

/* Internal representation of a screen cell */
typedef struct
{
  uint32_t chars[VTERM_MAX_CHARS_PER_CELL];
  ScreenPen pen;
} ScreenCell;

static int vterm_buffer_get_cell(const VTermScreen *screen, ScreenCell *buffer, VTermPos pos, VTermScreenCell *cell);
static void copy_ext_to_int_cell(ScreenCell *intcell, const VTermScreenCell *cell, int global_reverse);

struct VTermScreen
{
  VTerm *vt;
  VTermState *state;

  const VTermScreenCallbacks *callbacks;
  void *cbdata;

  VTermDamageSize damage_merge;
  /* start_row == -1 => no damage */
  VTermRect damaged;
  VTermRect pending_scrollrect;
  int pending_scroll_downward, pending_scroll_rightward;

  int rows;
  int cols;
  int global_reverse;

  /* Primary and Altscreen. buffers[1] is lazily allocated as needed */
  ScreenCell *buffers[2];

  /* buffer will == buffers[0] or buffers[1], depending on altscreen */
  ScreenCell *buffer;

  ScreenPen pen;
};

static inline ScreenCell *get_buffer_cell(const VTermScreen *screen, ScreenCell *buffer, int row, int col)
{
  if(row < 0 || row >= screen->rows)
    return NULL;
  if(col < 0 || col >= screen->cols)
    return NULL;
  return buffer + (screen->cols * row) + col;
}

static inline ScreenCell *getcell(const VTermScreen *screen, int row, int col)
{
  return get_buffer_cell(screen, screen->buffer, row, col);
}

static int get_last_filled_col(const VTermScreen *screen, const ScreenCell *buffer, int row, int col)
{
  int last_filled_col = -1;
  for(int c = col; c < screen->cols; c++) {
    if(buffer[row * screen->cols + c].chars[0])
      last_filled_col = c;
  }
  return last_filled_col;
}

VTermScreenLine *screen_line_alloc(VTermScreen *screen, size_t len)
{
  VTermScreenLine *line = vterm_allocator_malloc(screen->vt,
      sizeof(size_t) + sizeof(VTermScreenCell) * len);
  line->len = len;
  return line;
}

void screen_line_free(VTermScreen *screen, VTermScreenLine *line)
{
  vterm_allocator_free(screen->vt, line);
}

static ScreenCell *alloc_buffer(VTermScreen *screen, int new_rows, int new_cols)
{
  ScreenCell *new_buffer = vterm_allocator_malloc(screen->vt, sizeof(ScreenCell) * new_rows * new_cols);
  for(int row = 0; row < new_rows; row++) {
    for(int col = 0; col < new_cols; col++) {
      ScreenCell *new_cell = &new_buffer[row*new_cols + col];
      new_cell->chars[0] = 0;
      new_cell->pen = screen->pen;
    }
  }
  return new_buffer;
}

/* Get number of rows a line takes up (round up after dividing by width, with a minimum of 1) */
/* XXX deal with char width */
int get_line_row_count(VTermScreenLine *line, int width)
{
  int total_rows = (line->len + width - 1) / width;
  LBOUND(total_rows, 1);
  return total_rows;
}

static ScreenCell *reflow_buffer(VTermScreen *screen, ScreenCell *buffer, int new_rows, int new_cols)
{
  ScreenCell *new_buffer = vterm_allocator_malloc(screen->vt, sizeof(ScreenCell) * new_rows * new_cols);

  /* No, we don't deal with clients that only give one of push/pop */
  int can_use_sb = (buffer == screen->buffer && screen->callbacks &&
      screen->callbacks->sb_pushline && screen->callbacks->sb_popline);

  if(buffer == screen->buffer)
    DEBUG_LOG("reflow_buffer:\n");

  /* Gather source lines for the new buffer, until we get enough rows to fill it (or run out of
   * scrollback). This involves reading rows from the source buffer and converting them into
   * VTermScreenLines based on the wraparound bits, and possibly popping more lines from
   * scrollback to fill in the top of the new buffer (if it's bigger now). That way we can just
   * write into the new buffer as if everything is from scrollback. This method is a
   * bit more expensive in terms of allocations and copying, but is much simpler than having
   * the logic for reading from either the source buffer or scrollback lines all intertwined
   * with the logic for writing the cells into the destination buffer (...at least in C). */

  /* The src_lines buffer only needs to store up to <new_rows> lines, since each line is at least
   * one row. */
  VTermScreenLine **src_lines = vterm_allocator_malloc(screen->vt, sizeof(VTermScreenLine) * new_rows);
  int *line_row_counts = vterm_allocator_malloc(screen->vt, sizeof(int) * new_rows);
  int n_src_lines = 0, n_used_rows = 0;
  int src_row_end = screen->rows - 1;
  while(n_used_rows < new_rows) {
    VTermScreenLine *line = NULL;
    /* Are there still rows left in the source buffer? */
    if(src_row_end >= 0) {
      int src_row_start = src_row_end;
      /* Go back through rows in the source buffer until we find one that's not a wraparound */
      for(; src_row_start >= 0; src_row_start--)
        if(!buffer[src_row_start * screen->cols + 0].pen.wraparound)
          break;

      /* Remember whether we need to wraparound from scrollback */
      int first_line_wraparound = 0;
      if(src_row_start == -1) {
        first_line_wraparound = 1;
        src_row_start = 0;
      }

      /* See how many characters are in the last row so we know how many rows in the new
       * geometry to use for this line */
      int last_filled_col = get_last_filled_col(screen, buffer, src_row_end, 0);
      /* Always add one, since last_filled_col is one less than the number of columns used */
      last_filled_col++;
      /* XXX look for wide characters in the last column of the old screen, different wrap
       * points can make this calculation wrong in edge cases */
      int total_cols = (src_row_end - src_row_start) * screen->cols + last_filled_col;

      int n_cells = 0;
      /* Annoying case: a scrollback line continues onto the visible screen. Pop the line
       * if we can, and join it with the rows here. Any part of the line that isn't visible
       * in the new screen will get pushed into scrollback later.*/
      if(first_line_wraparound && can_use_sb) {
        VTermScreenLine *sb_line = screen->callbacks->sb_popline(screen->cbdata);
        if(sb_line) {
          total_cols += sb_line->len;
          line = screen_line_alloc(screen, total_cols);

          /* Copy data from the scrollback line into the new combined one, updating n_cells */
          for(; n_cells < sb_line->len; n_cells++)
            line->cells[n_cells] = sb_line->cells[n_cells];

          screen_line_free(screen, sb_line);
        }
      }

      /* If we didn't get a scrollback line, we still need to allocate a buffer */
      if(!line)
        line = screen_line_alloc(screen, total_cols);

      /* And now copy the source cells */
      for(int src_row = src_row_start; src_row <= src_row_end; src_row++) {
        int max_col = src_row == src_row_end ? last_filled_col : screen->cols;
        for(int src_col = 0; src_col < max_col; src_col++) {
          ASSERT(n_cells < line->len);
          /* XXX take width into account */
          VTermPos pos = { src_row, src_col };
          vterm_buffer_get_cell(screen, buffer, pos, &line->cells[n_cells++]);
          /* XXX clear wraparound bit */
        }
      }

      src_row_end = src_row_start - 1;
    }
    /* No more rows in the source buffer -> pop some scrollback */
    else if(can_use_sb)
      line = screen->callbacks->sb_popline(screen->cbdata);

    if(!line)
      break;

    /* Add this line into our array, and account for how many rows it will take up */
    ASSERT(n_src_lines < new_rows);
    src_lines[n_src_lines] = line;

    int row_count = get_line_row_count(line, new_cols);
    line_row_counts[n_src_lines] = row_count;
    n_used_rows += row_count;

    n_src_lines++;
  }

  /* Mark how many rows at the top we don't have lines for, so we can scroll down and move the cursor. */
  UBOUND(n_used_rows, new_rows);
  int offset = new_rows - n_used_rows;

  /* Write the lines to the destination, from the bottom up */
  int src_line_idx = 0;
  for(int dest_row_end = new_rows - 1 - offset; dest_row_end >= 0; ) {
    VTermScreenLine *src_line = NULL;
    int row_count = 1;
    if(src_line_idx < n_src_lines) {
      src_line = src_lines[src_line_idx];
      row_count = line_row_counts[src_line_idx];
      src_line_idx++;
    }

    int dest_row_start = dest_row_end - row_count + 1, src_idx = 0;

    /* Check for a partial line at the top of the screen. If we can't display the whole line,
     * push the invisible part into scrollback. */
    if(dest_row_start < 0) {
      /* Skip over the invisible part of the line */
      /* XXX deal with char width */
      src_idx = new_cols * -dest_row_start;
      dest_row_start = 0;

      if(can_use_sb) {
        VTermScreenLine *sb_line = screen_line_alloc(screen, src_idx);

        for(int n = 0; n < src_idx; n++)
          sb_line->cells[n] = src_line->cells[n];

        (screen->callbacks->sb_pushline)(sb_line, screen->cbdata);
      }
    }

    for(int dest_row = dest_row_start; dest_row <= dest_row_end; dest_row++) {
      for(int dest_col = 0; dest_col < new_cols; dest_col++) {
        ScreenCell *new_cell = &new_buffer[dest_row*new_cols + dest_col];

        if(src_line && src_idx < src_line->len)
          /* XXX deal with char width */
          copy_ext_to_int_cell(new_cell, &src_line->cells[src_idx++], screen->global_reverse);
        else {
          new_cell->chars[0] = 0;
          new_cell->pen = screen->pen;
        }

        /* Set the wraparound bit if needed for this new size */
        if (dest_col == 0 && dest_row > dest_row_start)
          new_cell->pen.wraparound = 1;
      }
    }

    dest_row_end = dest_row_start - 1;
  }

  /* Push all the necessary lines into scrollback */
  while(src_line_idx < n_src_lines) {
    VTermScreenLine *line = src_lines[src_line_idx];
    src_line_idx++;
    if(can_use_sb)
      (screen->callbacks->sb_pushline)(line, screen->cbdata);
    else
      screen_line_free(screen, line);
  }

  /* XXX double check that every src_line has been freed */
  vterm_allocator_free(screen->vt, src_lines);
  vterm_allocator_free(screen->vt, line_row_counts);

  vterm_allocator_free(screen->vt, buffer);

  return new_buffer;
}

static void damagerect(VTermScreen *screen, VTermRect rect)
{
  VTermRect emit;

  switch(screen->damage_merge) {
  case VTERM_DAMAGE_CELL:
    /* Always emit damage event */
    emit = rect;
    break;

  case VTERM_DAMAGE_ROW:
    /* Emit damage longer than one row. Try to merge with existing damage in
     * the same row */
    if(rect.end_row > rect.start_row + 1) {
      // Bigger than 1 line - flush existing, emit this
      vterm_screen_flush_damage(screen);
      emit = rect;
    }
    else if(screen->damaged.start_row == -1) {
      // None stored yet
      screen->damaged = rect;
      return;
    }
    else if(rect.start_row == screen->damaged.start_row) {
      // Merge with the stored line
      if(screen->damaged.start_col > rect.start_col)
        screen->damaged.start_col = rect.start_col;
      if(screen->damaged.end_col < rect.end_col)
        screen->damaged.end_col = rect.end_col;
      return;
    }
    else {
      // Emit the currently stored line, store a new one
      emit = screen->damaged;
      screen->damaged = rect;
    }
    break;

  case VTERM_DAMAGE_SCREEN:
  case VTERM_DAMAGE_SCROLL:
    /* Never emit damage event */
    if(screen->damaged.start_row == -1)
      screen->damaged = rect;
    else {
      rect_expand(&screen->damaged, &rect);
    }
    return;

  default:
    DEBUG_LOG("TODO: Maybe merge damage for level %d\n", screen->damage_merge);
    return;
  }

  if(screen->callbacks && screen->callbacks->damage)
    (*screen->callbacks->damage)(emit, screen->cbdata);
}

static void damagescreen(VTermScreen *screen)
{
  VTermRect rect = {
    .start_row = 0,
    .end_row   = screen->rows,
    .start_col = 0,
    .end_col   = screen->cols,
  };

  damagerect(screen, rect);
}

static int putglyph(VTermGlyphInfo *info, VTermPos pos, void *user)
{
  VTermScreen *screen = user;
  ScreenCell *cell = getcell(screen, pos.row, pos.col);

  if(!cell)
    return 0;

  /* Save the old wraparound bit of the cell, we don't want to overwrite it after
   * e.g. a BS or CR. The bit will be cleared when allocating new cells/lines in the terminal. */
  int save_wraparound = cell->pen.wraparound;

  int i;
  for(i = 0; i < VTERM_MAX_CHARS_PER_CELL && info->chars[i]; i++) {
    cell->chars[i] = info->chars[i];
    cell->pen = screen->pen;
  }
  if(i < VTERM_MAX_CHARS_PER_CELL)
    cell->chars[i] = 0;

  for(int col = 1; col < info->width; col++)
    getcell(screen, pos.row, pos.col + col)->chars[0] = (uint32_t)-1;

  VTermRect rect = {
    .start_row = pos.row,
    .end_row   = pos.row+1,
    .start_col = pos.col,
    .end_col   = pos.col+info->width,
  };

  cell->pen.protected_cell = info->protected_cell;
  cell->pen.dwl            = info->dwl;
  cell->pen.dhl            = info->dhl;
  /* Restore old wraparound bit */
  cell->pen.wraparound     = save_wraparound;

  damagerect(screen, rect);

  return 1;
}

static int moverect_internal(VTermRect dest, VTermRect src, void *user)
{
  VTermScreen *screen = user;

  if(screen->callbacks && screen->callbacks->sb_pushline &&
     dest.start_row == 0 && dest.start_col == 0 &&  // starts top-left corner
     dest.end_col == screen->cols &&                // full width
     screen->buffer == screen->buffers[0]) {        // not altscreen
    VTermPos pos;
    for(pos.row = 0; pos.row < src.start_row; pos.row++) {
      VTermScreenLine *line = NULL;
      /* XXX combine this with code in reflow_buffer() */
      int total_cols = screen->cols, n_cells = 0;
      if(getcell(screen, pos.row, 0)->pen.wraparound) {
        VTermScreenLine *sb_line = screen->callbacks->sb_popline(screen->cbdata);
        if(sb_line) {
          total_cols += sb_line->len;
          line = screen_line_alloc(screen, total_cols);

          /* Copy data from the scrollback line into the new combined one, updating n_cells */
          for(; n_cells < sb_line->len; n_cells++)
            line->cells[n_cells] = sb_line->cells[n_cells];

          screen_line_free(screen, sb_line);
        }
      }
      /* If we didn't get a scrollback line, we still need to allocate a buffer */
      if(!line)
        line = screen_line_alloc(screen, total_cols);

      for(pos.col = 0; pos.col < screen->cols; pos.col++)
        vterm_screen_get_cell(screen, pos, &line->cells[n_cells + pos.col]);

      /* Pass the buffer to the library consumer. They are responsible
       * for the memory after sb_pushline, and must either free it themselves or
       * pass it back to us through a later sb_popline. */
      (screen->callbacks->sb_pushline)(line, screen->cbdata);
    }
  }

  int cols = src.end_col - src.start_col;
  int downward = src.start_row - dest.start_row;

  int init_row, test_row, inc_row;
  if(downward < 0) {
    init_row = dest.end_row - 1;
    test_row = dest.start_row - 1;
    inc_row  = -1;
  }
  else {
    init_row = dest.start_row;
    test_row = dest.end_row;
    inc_row  = +1;
  }

  for(int row = init_row; row != test_row; row += inc_row)
    memmove(getcell(screen, row, dest.start_col),
            getcell(screen, row + downward, src.start_col),
            cols * sizeof(ScreenCell));

  return 1;
}

static int moverect_user(VTermRect dest, VTermRect src, void *user)
{
  VTermScreen *screen = user;

  if(screen->callbacks && screen->callbacks->moverect) {
    if(screen->damage_merge != VTERM_DAMAGE_SCROLL)
      // Avoid an infinite loop
      vterm_screen_flush_damage(screen);

    if((*screen->callbacks->moverect)(dest, src, screen->cbdata))
      return 1;
  }

  damagerect(screen, dest);

  return 1;
}

static int erase_internal(VTermRect rect, int selective, void *user)
{
  VTermScreen *screen = user;

  for(int row = rect.start_row; row < screen->state->rows && row < rect.end_row; row++) {
    const VTermLineInfo *info = vterm_state_get_lineinfo(screen->state, row);

    for(int col = rect.start_col; col < rect.end_col; col++) {
      ScreenCell *cell = getcell(screen, row, col);

      if(selective && cell->pen.protected_cell)
        continue;

      cell->chars[0] = 0;
      cell->pen = screen->pen;
      cell->pen.dwl = info->doublewidth;
      cell->pen.dhl = info->doubleheight;
    }
  }

  return 1;
}

static int erase_user(VTermRect rect, int selective, void *user)
{
  VTermScreen *screen = user;

  damagerect(screen, rect);

  return 1;
}

static int erase(VTermRect rect, int selective, void *user)
{
  erase_internal(rect, selective, user);
  return erase_user(rect, 0, user);
}

static int scrollrect(VTermRect rect, int downward, int rightward, void *user)
{
  VTermScreen *screen = user;

  if(screen->damage_merge != VTERM_DAMAGE_SCROLL) {
    vterm_scroll_rect(rect, downward, rightward,
        moverect_internal, erase_internal, screen);

    vterm_screen_flush_damage(screen);

    vterm_scroll_rect(rect, downward, rightward,
        moverect_user, erase_user, screen);

    return 1;
  }

  if(screen->damaged.start_row != -1 &&
     !rect_intersects(&rect, &screen->damaged)) {
    vterm_screen_flush_damage(screen);
  }

  if(screen->pending_scrollrect.start_row == -1) {
    screen->pending_scrollrect = rect;
    screen->pending_scroll_downward  = downward;
    screen->pending_scroll_rightward = rightward;
  }
  else if(rect_equal(&screen->pending_scrollrect, &rect) &&
     ((screen->pending_scroll_downward  == 0 && downward  == 0) ||
      (screen->pending_scroll_rightward == 0 && rightward == 0))) {
    screen->pending_scroll_downward  += downward;
    screen->pending_scroll_rightward += rightward;
  }
  else {
    vterm_screen_flush_damage(screen);

    screen->pending_scrollrect = rect;
    screen->pending_scroll_downward  = downward;
    screen->pending_scroll_rightward = rightward;
  }

  vterm_scroll_rect(rect, downward, rightward,
      moverect_internal, erase_internal, screen);

  if(screen->damaged.start_row == -1)
    return 1;

  if(rect_contains(&rect, &screen->damaged)) {
    /* Scroll region entirely contains the damage; just move it */
    vterm_rect_move(&screen->damaged, -downward, -rightward);
    rect_clip(&screen->damaged, &rect);
  }
  /* There are a number of possible cases here, but lets restrict this to only
   * the common case where we might actually gain some performance by
   * optimising it. Namely, a vertical scroll that neatly cuts the damage
   * region in half.
   */
  else if(rect.start_col <= screen->damaged.start_col &&
          rect.end_col   >= screen->damaged.end_col &&
          rightward == 0) {
    if(screen->damaged.start_row >= rect.start_row &&
       screen->damaged.start_row  < rect.end_row) {
      screen->damaged.start_row -= downward;
      if(screen->damaged.start_row < rect.start_row)
        screen->damaged.start_row = rect.start_row;
      if(screen->damaged.start_row > rect.end_row)
        screen->damaged.start_row = rect.end_row;
    }
    if(screen->damaged.end_row >= rect.start_row &&
       screen->damaged.end_row  < rect.end_row) {
      screen->damaged.end_row -= downward;
      if(screen->damaged.end_row < rect.start_row)
        screen->damaged.end_row = rect.start_row;
      if(screen->damaged.end_row > rect.end_row)
        screen->damaged.end_row = rect.end_row;
    }
  }
  else {
    DEBUG_LOG("TODO: Just flush and redo damaged=" STRFrect " rect=" STRFrect "\n",
        ARGSrect(screen->damaged), ARGSrect(rect));
  }

  return 1;
}

static int movecursor(VTermPos pos, VTermPos oldpos, int visible, void *user)
{
  VTermScreen *screen = user;

  if(screen->callbacks && screen->callbacks->movecursor)
    return (*screen->callbacks->movecursor)(pos, oldpos, visible, screen->cbdata);

  return 0;
}

static int setpenattr(VTermAttr attr, VTermValue *val, void *user)
{
  VTermScreen *screen = user;

  switch(attr) {
  case VTERM_ATTR_BOLD:
    screen->pen.bold = val->boolean;
    return 1;
  case VTERM_ATTR_UNDERLINE:
    screen->pen.underline = val->number;
    return 1;
  case VTERM_ATTR_ITALIC:
    screen->pen.italic = val->boolean;
    return 1;
  case VTERM_ATTR_BLINK:
    screen->pen.blink = val->boolean;
    return 1;
  case VTERM_ATTR_REVERSE:
    screen->pen.reverse = val->boolean;
    return 1;
  case VTERM_ATTR_STRIKE:
    screen->pen.strike = val->boolean;
    return 1;
  case VTERM_ATTR_FONT:
    screen->pen.font = val->number;
    return 1;
  case VTERM_ATTR_FOREGROUND:
    screen->pen.fg = val->color;
    return 1;
  case VTERM_ATTR_BACKGROUND:
    screen->pen.bg = val->color;
    return 1;

  case VTERM_N_ATTRS:
    return 0;
  }

  return 0;
}

static int settermprop(VTermProp prop, VTermValue *val, void *user)
{
  VTermScreen *screen = user;

  switch(prop) {
  case VTERM_PROP_ALTSCREEN:
    if(val->boolean && !screen->buffers[1])
      return 0;

    screen->buffer = val->boolean ? screen->buffers[1] : screen->buffers[0];
    /* only send a damage event on disable; because during enable there's an
     * erase that sends a damage anyway
     */
    if(!val->boolean)
      damagescreen(screen);
    break;
  case VTERM_PROP_REVERSE:
    screen->global_reverse = val->boolean;
    damagescreen(screen);
    break;
  default:
    ; /* ignore */
  }

  if(screen->callbacks && screen->callbacks->settermprop)
    return (*screen->callbacks->settermprop)(prop, val, screen->cbdata);

  return 1;
}

static int bell(void *user)
{
  VTermScreen *screen = user;

  if(screen->callbacks && screen->callbacks->bell)
    return (*screen->callbacks->bell)(screen->cbdata);

  return 0;
}

static int resize(int new_rows, int new_cols, VTermPos *delta, void *user)
{
  VTermScreen *screen = user;

  int is_altscreen = (screen->buffers[1] && screen->buffer == screen->buffers[1]);

  int old_rows = screen->rows;
  int old_cols = screen->cols;

  if(!is_altscreen && new_rows < old_rows) {
    // Fewer rows - determine if we're going to scroll at all, and if so, push
    // those lines to scrollback
    VTermPos pos = { 0, 0 };
    VTermPos cursor = screen->state->pos;
    // Find the first blank row after the cursor.
    for(pos.row = old_rows - 1; pos.row >= new_rows; pos.row--)
      if(!vterm_screen_is_eol(screen, pos) || cursor.row == pos.row)
        break;

    int first_blank_row = pos.row + 1;

    VTermRect rect = {
      .start_row = 0,
      .end_row   = old_rows,
      .start_col = 0,
      .end_col   = old_cols,
    };
    /* First, scroll down if we need to. This is to push any lines into the scrollback
     * that will be cutoff from the resize. */
    if(first_blank_row > new_rows) {
      scrollrect(rect, first_blank_row - new_rows, 0, user);

      delta->row -= first_blank_row - new_rows;
    }

    /* Then, scroll up. This is because reflow_buffer() keeps lines stuck to the bottom,
     * and so we need to move the actual last line to the last line of the old buffer */
    int offset = (old_rows - new_rows);
    rect.end_row = new_rows;
    scrollrect(rect, -offset, 0, user);

    DEBUG_LOG("resize shrink: r=(%d->%d) cur=(%d,%d) fb=%d dr=%d\n",
        old_rows, new_rows, cursor.row, cursor.col, first_blank_row, delta->row);
    vterm_screen_flush_damage(screen);
  }

  /* XXX pass in cursor */
  screen->buffers[0] = reflow_buffer(screen, screen->buffers[0], new_rows, new_cols);

  if(screen->buffers[1])
    screen->buffers[1] = reflow_buffer(screen, screen->buffers[1], new_rows, new_cols);

  screen->buffer = is_altscreen ? screen->buffers[1] : screen->buffers[0];

  screen->rows = new_rows;
  screen->cols = new_cols;

  /* Just damage the entire screen. Reflow is too complicated to salvage anything here. */
  damagescreen(screen);
  vterm_screen_flush_damage(screen);

  DEBUG_LOG("resized: r=(%d->%d) c=(%d->%d) d=(%d,%d)\n",
      old_rows, new_rows, old_cols, new_cols, delta->row, delta->col);

  if(screen->callbacks && screen->callbacks->resize)
    return (*screen->callbacks->resize)(new_rows, new_cols, screen->cbdata);

  return 1;
}

static int markwraparound(VTermPos pos, void *user)
{
  VTermScreen *screen = user;
  ScreenCell *cell = getcell(screen, pos.row, 0);

  cell->pen.wraparound = 1;
  return 1;
}

static int setlineinfo(int row, const VTermLineInfo *newinfo, const VTermLineInfo *oldinfo, void *user)
{
  VTermScreen *screen = user;

  if(newinfo->doublewidth != oldinfo->doublewidth ||
     newinfo->doubleheight != oldinfo->doubleheight) {
    for(int col = 0; col < screen->cols; col++) {
      ScreenCell *cell = getcell(screen, row, col);
      cell->pen.dwl = newinfo->doublewidth;
      cell->pen.dhl = newinfo->doubleheight;
    }

    VTermRect rect = {
      .start_row = row,
      .end_row   = row + 1,
      .start_col = 0,
      .end_col   = newinfo->doublewidth ? screen->cols / 2 : screen->cols,
    };
    damagerect(screen, rect);

    if(newinfo->doublewidth) {
      rect.start_col = screen->cols / 2;
      rect.end_col   = screen->cols;

      erase_internal(rect, 0, user);
    }
  }

  return 1;
}

static VTermStateCallbacks state_cbs = {
  .putglyph    = &putglyph,
  .movecursor  = &movecursor,
  .scrollrect  = &scrollrect,
  .erase       = &erase,
  .setpenattr  = &setpenattr,
  .settermprop = &settermprop,
  .bell        = &bell,
  .resize      = &resize,
  .setlineinfo = &setlineinfo,
  .markwraparound = &markwraparound,
};

static VTermScreen *screen_new(VTerm *vt)
{
  VTermState *state = vterm_obtain_state(vt);
  if(!state)
    return NULL;

  VTermScreen *screen = vterm_allocator_malloc(vt, sizeof(VTermScreen));
  int rows, cols;

  vterm_get_size(vt, &rows, &cols);

  screen->vt = vt;
  screen->state = state;

  screen->damage_merge = VTERM_DAMAGE_CELL;
  screen->damaged.start_row = -1;
  screen->pending_scrollrect.start_row = -1;

  screen->rows = rows;
  screen->cols = cols;

  screen->callbacks = NULL;
  screen->cbdata    = NULL;

  screen->buffers[0] = alloc_buffer(screen, rows, cols);

  screen->buffer = screen->buffers[0];

  vterm_state_set_callbacks(screen->state, &state_cbs, screen);

  return screen;
}

INTERNAL void vterm_screen_free(VTermScreen *screen)
{
  vterm_allocator_free(screen->vt, screen->buffers[0]);
  if(screen->buffers[1])
    vterm_allocator_free(screen->vt, screen->buffers[1]);

  vterm_allocator_free(screen->vt, screen);
}

void vterm_screen_reset(VTermScreen *screen, int hard)
{
  screen->damaged.start_row = -1;
  screen->pending_scrollrect.start_row = -1;
  vterm_state_reset(screen->state, hard);
  vterm_screen_flush_damage(screen);
}

static size_t _get_chars(const VTermScreen *screen, const int utf8, void *buffer, size_t len, const VTermRect rect)
{
  size_t outpos = 0;
  int padding = 0;

#define PUT(c)                                             \
  if(utf8) {                                               \
    size_t thislen = utf8_seqlen(c);                       \
    if(buffer && outpos + thislen <= len)                  \
      outpos += fill_utf8((c), (char *)buffer + outpos);   \
    else                                                   \
      outpos += thislen;                                   \
  }                                                        \
  else {                                                   \
    if(buffer && outpos + 1 <= len)                        \
      ((uint32_t*)buffer)[outpos++] = (c);                 \
    else                                                   \
      outpos++;                                            \
  }

  for(int row = rect.start_row; row < rect.end_row; row++) {
    for(int col = rect.start_col; col < rect.end_col; col++) {
      ScreenCell *cell = getcell(screen, row, col);

      if(cell->chars[0] == 0)
        // Erased cell, might need a space
        padding++;
      else if(cell->chars[0] == (uint32_t)-1)
        // Gap behind a double-width char, do nothing
        ;
      else {
        while(padding) {
          PUT(UNICODE_SPACE);
          padding--;
        }
        for(int i = 0; i < VTERM_MAX_CHARS_PER_CELL && cell->chars[i]; i++) {
          PUT(cell->chars[i]);
        }
      }
    }

    if(row < rect.end_row - 1) {
      PUT(UNICODE_LINEFEED);
      padding = 0;
    }
  }

  return outpos;
}

size_t vterm_screen_get_chars(const VTermScreen *screen, uint32_t *chars, size_t len, const VTermRect rect)
{
  return _get_chars(screen, 0, chars, len, rect);
}

size_t vterm_screen_get_text(const VTermScreen *screen, char *str, size_t len, const VTermRect rect)
{
  return _get_chars(screen, 1, str, len, rect);
}

/* Copy internal to external representation of a screen cell */
static int vterm_buffer_get_cell(const VTermScreen *screen, ScreenCell *buffer, VTermPos pos, VTermScreenCell *cell)
{
  ScreenCell *intcell = get_buffer_cell(screen, buffer, pos.row, pos.col);
  if(!intcell)
    return 0;

  for(int i = 0; ; i++) {
    cell->chars[i] = intcell->chars[i];
    if(!intcell->chars[i])
      break;
  }

  cell->attrs.bold      = intcell->pen.bold;
  cell->attrs.underline = intcell->pen.underline;
  cell->attrs.italic    = intcell->pen.italic;
  cell->attrs.blink     = intcell->pen.blink;
  cell->attrs.reverse   = intcell->pen.reverse ^ screen->global_reverse;
  cell->attrs.strike    = intcell->pen.strike;
  cell->attrs.font      = intcell->pen.font;

  cell->attrs.dwl = intcell->pen.dwl;
  cell->attrs.dhl = intcell->pen.dhl;

  cell->attrs.wraparound = intcell->pen.wraparound;

  cell->fg = intcell->pen.fg;
  cell->bg = intcell->pen.bg;

  if(pos.col < (screen->cols - 1) &&
     get_buffer_cell(screen, buffer, pos.row, pos.col + 1)->chars[0] == (uint32_t)-1)
    cell->width = 2;
  else
    cell->width = 1;

  return 1;
}

/* Convenience function for vterm_buffer_get_cell using screen->buffer as buffer */
int vterm_screen_get_cell(const VTermScreen *screen, VTermPos pos, VTermScreenCell *cell)
{
  return vterm_buffer_get_cell(screen, screen->buffer, pos, cell);
}

/* Copy external to internal representation of a screen cell */
/* static because it's only used internally for sb_popline during resize */
static void copy_ext_to_int_cell(ScreenCell *intcell, const VTermScreenCell *cell, int global_reverse)
{
  for(int i = 0; ; i++) {
    intcell->chars[i] = cell->chars[i];
    if(!cell->chars[i])
      break;
  }

  intcell->pen.bold      = cell->attrs.bold;
  intcell->pen.underline = cell->attrs.underline;
  intcell->pen.italic    = cell->attrs.italic;
  intcell->pen.blink     = cell->attrs.blink;
  intcell->pen.reverse   = cell->attrs.reverse ^ global_reverse;
  intcell->pen.strike    = cell->attrs.strike;
  intcell->pen.font      = cell->attrs.font;

  intcell->pen.fg = cell->fg;
  intcell->pen.bg = cell->bg;
}

int vterm_screen_is_eol(const VTermScreen *screen, VTermPos pos)
{
  int last_filled_col = get_last_filled_col(screen, screen->buffer, pos.row, pos.col);

  return (last_filled_col == -1);
}

VTermScreen *vterm_obtain_screen(VTerm *vt)
{
  if(vt->screen)
    return vt->screen;

  VTermScreen *screen = screen_new(vt);
  vt->screen = screen;

  return screen;
}

void vterm_screen_enable_altscreen(VTermScreen *screen, int altscreen)
{

  if(!screen->buffers[1] && altscreen) {
    int rows, cols;
    vterm_get_size(screen->vt, &rows, &cols);

    screen->buffers[1] = alloc_buffer(screen, rows, cols);
  }
}

void vterm_screen_set_callbacks(VTermScreen *screen, const VTermScreenCallbacks *callbacks, void *user)
{
  screen->callbacks = callbacks;
  screen->cbdata = user;
}

void *vterm_screen_get_cbdata(VTermScreen *screen)
{
  return screen->cbdata;
}

void vterm_screen_set_unrecognised_fallbacks(VTermScreen *screen, const VTermParserCallbacks *fallbacks, void *user)
{
  vterm_state_set_unrecognised_fallbacks(screen->state, fallbacks, user);
}

void *vterm_screen_get_unrecognised_fbdata(VTermScreen *screen)
{
  return vterm_state_get_unrecognised_fbdata(screen->state);
}

void vterm_screen_flush_damage(VTermScreen *screen)
{
  if(screen->pending_scrollrect.start_row != -1) {
    vterm_scroll_rect(screen->pending_scrollrect, screen->pending_scroll_downward, screen->pending_scroll_rightward,
        moverect_user, erase_user, screen);

    screen->pending_scrollrect.start_row = -1;
  }

  if(screen->damaged.start_row != -1) {
    if(screen->callbacks && screen->callbacks->damage)
      (*screen->callbacks->damage)(screen->damaged, screen->cbdata);

    screen->damaged.start_row = -1;
  }
}

void vterm_screen_set_damage_merge(VTermScreen *screen, VTermDamageSize size)
{
  vterm_screen_flush_damage(screen);
  screen->damage_merge = size;
}

static int attrs_differ(VTermAttrMask attrs, ScreenCell *a, ScreenCell *b)
{
  if((attrs & VTERM_ATTR_BOLD_MASK)       && (a->pen.bold != b->pen.bold))
    return 1;
  if((attrs & VTERM_ATTR_UNDERLINE_MASK)  && (a->pen.underline != b->pen.underline))
    return 1;
  if((attrs & VTERM_ATTR_ITALIC_MASK)     && (a->pen.italic != b->pen.italic))
    return 1;
  if((attrs & VTERM_ATTR_BLINK_MASK)      && (a->pen.blink != b->pen.blink))
    return 1;
  if((attrs & VTERM_ATTR_REVERSE_MASK)    && (a->pen.reverse != b->pen.reverse))
    return 1;
  if((attrs & VTERM_ATTR_STRIKE_MASK)     && (a->pen.strike != b->pen.strike))
    return 1;
  if((attrs & VTERM_ATTR_FONT_MASK)       && (a->pen.font != b->pen.font))
    return 1;
  if((attrs & VTERM_ATTR_FOREGROUND_MASK) && !vterm_color_equal(a->pen.fg, b->pen.fg))
    return 1;
  if((attrs & VTERM_ATTR_BACKGROUND_MASK) && !vterm_color_equal(a->pen.bg, b->pen.bg))
    return 1;

  return 0;
}

int vterm_screen_get_attrs_extent(const VTermScreen *screen, VTermRect *extent, VTermPos pos, VTermAttrMask attrs)
{
  ScreenCell *target = getcell(screen, pos.row, pos.col);

  // TODO: bounds check
  extent->start_row = pos.row;
  extent->end_row   = pos.row + 1;

  if(extent->start_col < 0)
    extent->start_col = 0;
  if(extent->end_col < 0)
    extent->end_col = screen->cols;

  int col;

  for(col = pos.col - 1; col >= extent->start_col; col--)
    if(attrs_differ(attrs, target, getcell(screen, pos.row, col)))
      break;
  extent->start_col = col + 1;

  for(col = pos.col + 1; col < extent->end_col; col++)
    if(attrs_differ(attrs, target, getcell(screen, pos.row, col)))
      break;
  extent->end_col = col - 1;

  return 1;
}

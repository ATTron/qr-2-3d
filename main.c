#include <ncurses.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "vendor/qrcodegen.h"

#define STB_IMAGE_IMPLEMENTATION
#include "vendor/stb_image.h"

#define STB_IMAGE_WRITE_IMPLEMENTATION
#include "vendor/stb_image_write.h"

#define SCALE 10
#define QUIET_ZONE 4

#define FMT_PNG 1
#define FMT_STL 2
#define FMT_OBJ 4
#define FMT_ALL (FMT_PNG | FMT_STL | FMT_OBJ)
#define LOGO_GRID_MAX                                                          \
  512 /* max grid cells on longest side for logo extrusion */

/* ── SVG conversion helper ───────────────────────────────────────── */

static bool has_svg_ext(const char *path) {
  size_t len = strlen(path);
  if (len < 4)
    return false;
  const char *ext = path + len - 4;
  return (strcasecmp(ext, ".svg") == 0);
}

/* Convert SVG to a temp PNG via rsvg-convert. Returns true and fills
   out_path with the temp file path on success. Caller should unlink. */
static bool convert_svg_to_png(const char *svg_path, char *out_path,
                               size_t out_size) {
  snprintf(out_path, out_size, "/tmp/qrgen_logo_%d.png", getpid());
  char cmd[1024];
  snprintf(
      cmd, sizeof(cmd),
      "rsvg-convert -w 512 -h 512 --keep-aspect-ratio '%s' -o '%s' 2>/dev/null",
      svg_path, out_path);
  if (system(cmd) == 0 && access(out_path, R_OK) == 0)
    return true;
  /* fallback: try magick */
  snprintf(cmd, sizeof(cmd), "magick '%s' -resize 512x512 '%s' 2>/dev/null",
           svg_path, out_path);
  if (system(cmd) == 0 && access(out_path, R_OK) == 0)
    return true;
  return false;
}

/* ── Logo grid for 3D extrusion ──────────────────────────────────── */

/* Load an image and convert to a boolean grid (true = filled pixel).
   max_dim is the longest side in grid cells; the other side is scaled
   to preserve aspect ratio. out_w/out_h receive the actual grid size. */
static bool *load_logo_grid(const char *path, int max_dim, int *out_w,
                            int *out_h) {
  int lw, lh, lc;
  unsigned char *img = stbi_load(path, &lw, &lh, &lc, 4);
  if (!img)
    return NULL;

  /* use source resolution, capped at max_dim */
  int gw, gh;
  if (lw >= lh) {
    gw = (lw < max_dim) ? lw : max_dim;
    gh = (int)((float)lh / lw * gw);
    if (gh < 1)
      gh = 1;
  } else {
    gh = (lh < max_dim) ? lh : max_dim;
    gw = (int)((float)lw / lh * gh);
    if (gw < 1)
      gw = 1;
  }

  bool *grid = calloc((size_t)gw * gh, sizeof(bool));
  if (!grid) {
    stbi_image_free(img);
    return NULL;
  }

  for (int y = 0; y < gh; y++) {
    for (int x = 0; x < gw; x++) {
      int sx = x * lw / gw;
      int sy = y * lh / gh;
      if (sx >= lw)
        sx = lw - 1;
      if (sy >= lh)
        sy = lh - 1;
      int si = (sy * lw + sx) * 4;
      unsigned char r = img[si], g = img[si + 1], b = img[si + 2],
                    a = img[si + 3];
      if (a >= 128 && (r < 240 || g < 240 || b < 240))
        grid[y * gw + x] = true;
    }
  }

  stbi_image_free(img);
  *out_w = gw;
  *out_h = gh;
  return grid;
}

static bool grid_filled(const bool *grid, int w, int h, int x, int y) {
  if (x < 0 || x >= w || y < 0 || y >= h)
    return false;
  return grid[y * w + x];
}

/* ── TUI helpers ─────────────────────────────────────────────────── */

static void tui_title(const char *title) {
  clear();
  attron(A_BOLD);
  mvprintw(1, 2, "%s", title);
  attroff(A_BOLD);
}

static void tui_prompt(int row, const char *prompt, char *buf, int bufsize) {
  mvprintw(row, 4, "%s: ", prompt);
  echo();
  curs_set(1);
  getnstr(buf, bufsize - 1);
  noecho();
  curs_set(0);
}

/* ── STL helpers ─────────────────────────────────────────────────── */

static void write_triangle_stl(FILE *f, float nx, float ny, float nz, float x1,
                               float y1, float z1, float x2, float y2, float z2,
                               float x3, float y3, float z3) {
  float normal[3] = {nx, ny, nz};
  float v1[3] = {x1, y1, z1};
  float v2[3] = {x2, y2, z2};
  float v3[3] = {x3, y3, z3};
  uint16_t attr = 0;
  fwrite(normal, sizeof(float), 3, f);
  fwrite(v1, sizeof(float), 3, f);
  fwrite(v2, sizeof(float), 3, f);
  fwrite(v3, sizeof(float), 3, f);
  fwrite(&attr, sizeof(uint16_t), 1, f);
}

/* ── OBJ helpers ─────────────────────────────────────────────────── */

static void write_quad_obj(FILE *f, float x1, float y1, float z1, float x2,
                           float y2, float z2, float x3, float y3, float z3,
                           float x4, float y4, float z4, int *vi) {
  int b = *vi;
  fprintf(f, "v %f %f %f\n", x1, y1, z1);
  fprintf(f, "v %f %f %f\n", x2, y2, z2);
  fprintf(f, "v %f %f %f\n", x3, y3, z3);
  fprintf(f, "v %f %f %f\n", x4, y4, z4);
  fprintf(f, "f %d %d %d %d\n", b + 1, b + 2, b + 3, b + 4);
  *vi += 4;
}

/* ── Export functions ────────────────────────────────────────────── */

static bool export_png(const uint8_t *qr, int qr_size, const char *path,
                       const char *logo_path) {
  int img_modules = qr_size + QUIET_ZONE * 2;
  int img_px = img_modules * SCALE;
  unsigned char *pixels = calloc((size_t)img_px * img_px * 4, 1);
  if (!pixels)
    return false;

  /* white background */
  for (int i = 0; i < img_px * img_px; i++) {
    pixels[i * 4 + 0] = 255;
    pixels[i * 4 + 1] = 255;
    pixels[i * 4 + 2] = 255;
    pixels[i * 4 + 3] = 255;
  }

  /* draw QR modules */
  for (int y = 0; y < qr_size; y++) {
    for (int x = 0; x < qr_size; x++) {
      if (qrcodegen_getModule(qr, x, y)) {
        int px0 = (x + QUIET_ZONE) * SCALE;
        int py0 = (y + QUIET_ZONE) * SCALE;
        for (int dy = 0; dy < SCALE; dy++) {
          for (int dx = 0; dx < SCALE; dx++) {
            int idx = ((py0 + dy) * img_px + (px0 + dx)) * 4;
            pixels[idx + 0] = 0;
            pixels[idx + 1] = 0;
            pixels[idx + 2] = 0;
            pixels[idx + 3] = 255;
          }
        }
      }
    }
  }

  /* logo overlay — composited at pixel level for PNG */
  if (logo_path && logo_path[0]) {
    int lw, lh, lc;
    unsigned char *logo = stbi_load(logo_path, &lw, &lh, &lc, 4);
    if (logo) {
      int target_w = img_px / 4;
      int target_h = (int)((float)lh / lw * target_w);
      int ox = (img_px - target_w) / 2;
      int oy = (img_px - target_h) / 2;

      /* clear white zone behind logo */
      int pad = 4;
      for (int y = oy - pad; y < oy + target_h + pad; y++) {
        for (int x = ox - pad; x < ox + target_w + pad; x++) {
          if (x >= 0 && x < img_px && y >= 0 && y < img_px) {
            int idx = (y * img_px + x) * 4;
            pixels[idx + 0] = 255;
            pixels[idx + 1] = 255;
            pixels[idx + 2] = 255;
            pixels[idx + 3] = 255;
          }
        }
      }

      /* nearest-neighbor scale and composite */
      for (int y = 0; y < target_h; y++) {
        for (int x = 0; x < target_w; x++) {
          int sx = x * lw / target_w;
          int sy = y * lh / target_h;
          int si = (sy * lw + sx) * 4;
          int di = ((oy + y) * img_px + (ox + x)) * 4;
          unsigned char a = logo[si + 3];
          if (a == 255) {
            pixels[di + 0] = logo[si + 0];
            pixels[di + 1] = logo[si + 1];
            pixels[di + 2] = logo[si + 2];
            pixels[di + 3] = 255;
          } else if (a > 0) {
            for (int c = 0; c < 3; c++) {
              pixels[di + c] = (unsigned char)((logo[si + c] * a +
                                                pixels[di + c] * (255 - a)) /
                                               255);
            }
            pixels[di + 3] = 255;
          }
        }
      }
      stbi_image_free(logo);
    } else {
      fprintf(stderr, "Warning: could not load logo '%s'\n", logo_path);
    }
  }

  int ok = stbi_write_png(path, img_px, img_px, 4, pixels, img_px * 4);
  free(pixels);
  return ok != 0;
}

/* ── Shared setup for 3D exports ─────────────────────────────────── */

static void setup_logo(const char *logo_path, int qr_size, bool **out_grid,
                       int *out_gw, int *out_gh, float *out_cell, int *out_lmx0,
                       int *out_lmy0, int *out_lmw, int *out_lmh,
                       bool *out_has) {
  *out_grid = NULL;
  *out_gw = 0;
  *out_gh = 0;
  *out_cell = 0;
  *out_lmx0 = 0;
  *out_lmy0 = 0;
  *out_lmw = 0;
  *out_lmh = 0;
  *out_has = false;
  if (!logo_path || !logo_path[0])
    return;

  int logo_max_mods = qr_size / 4;
  if (logo_max_mods < 3)
    logo_max_mods = 3;
  *out_grid = load_logo_grid(logo_path, LOGO_GRID_MAX, out_gw, out_gh);
  if (!*out_grid)
    return;
  *out_has = true;
  int gw = *out_gw, gh = *out_gh;

  /* flip grid vertically: image y goes top-down, 3D y goes bottom-up */
  for (int y = 0; y < gh / 2; y++) {
    int y2 = gh - 1 - y;
    for (int x = 0; x < gw; x++) {
      bool tmp = (*out_grid)[y * gw + x];
      (*out_grid)[y * gw + x] = (*out_grid)[y2 * gw + x];
      (*out_grid)[y2 * gw + x] = tmp;
    }
  }
  int long_side = (gw >= gh) ? gw : gh;
  *out_cell = (float)logo_max_mods / long_side;
  if (gw >= gh) {
    *out_lmw = logo_max_mods;
    *out_lmh = (gh * logo_max_mods + gw - 1) / gw;
  } else {
    *out_lmh = logo_max_mods;
    *out_lmw = (gw * logo_max_mods + gh - 1) / gh;
  }
  *out_lmx0 = (qr_size - *out_lmw) / 2;
  *out_lmy0 = (qr_size - *out_lmh) / 2;
}

static bool *build_present_map(const uint8_t *qr, int n, bool has_logo,
                               int lmx0, int lmy0, int lmw, int lmh) {
  bool *present = calloc((size_t)n * n, sizeof(bool));
  if (!present)
    return NULL;
  for (int y = 0; y < n; y++)
    for (int x = 0; x < n; x++) {
      if (!qrcodegen_getModule(qr, x, y))
        continue;
      if (has_logo && x >= lmx0 && x < lmx0 + lmw && y >= lmy0 &&
          y < lmy0 + lmh)
        continue;
      present[y * n + x] = true;
    }
  return present;
}

/* Voxel fill check: 2-layer grid.
   Layer 0 (z=0..0.5): always filled.
   Layer 1 (z=0.5..1.5): filled if present[]. */
static bool vfill(const bool *present, int n, int vx, int vy, int vz) {
  if (vx < 0 || vx >= n || vy < 0 || vy >= n || vz < 0 || vz >= 2)
    return false;
  return (vz == 0) || present[vy * n + vx];
}

/* ── STL export (voxel-based) ────────────────────────────────────── */

static bool export_stl(const uint8_t *qr, int qr_size, const char *path,
                       const char *logo_path) {
  FILE *f = fopen(path, "wb");
  if (!f)
    return false;
  int n = qr_size;

  bool *grid;
  int gw, gh, lmx0, lmy0, lmw, lmh;
  float cell;
  bool has_logo;
  setup_logo(logo_path, n, &grid, &gw, &gh, &cell, &lmx0, &lmy0, &lmw, &lmh,
             &has_logo);

  bool *present = build_present_map(qr, n, has_logo, lmx0, lmy0, lmw, lmh);
  if (!present) {
    free(grid);
    fclose(f);
    return false;
  }

  float zv[3] = {0.0f, 0.5f, 1.5f};

  /* count exposed voxel faces */
  uint32_t fc = 0;
  for (int vz = 0; vz < 2; vz++)
    for (int vy = 0; vy < n; vy++)
      for (int vx = 0; vx < n; vx++) {
        if (!vfill(present, n, vx, vy, vz))
          continue;
        if (!vfill(present, n, vx, vy, vz - 1))
          fc++;
        if (!vfill(present, n, vx, vy, vz + 1))
          fc++;
        if (!vfill(present, n, vx - 1, vy, vz))
          fc++;
        if (!vfill(present, n, vx + 1, vy, vz))
          fc++;
        if (!vfill(present, n, vx, vy - 1, vz))
          fc++;
        if (!vfill(present, n, vx, vy + 1, vz))
          fc++;
      }

  /* logo faces (independent closed boxes) */
  uint32_t lf = 0;
  if (has_logo)
    for (int gy = 0; gy < gh; gy++)
      for (int gx = 0; gx < gw; gx++) {
        if (!grid[gy * gw + gx])
          continue;
        lf += 2; /* top + bottom */
        if (!grid_filled(grid, gw, gh, gx - 1, gy))
          lf++;
        if (!grid_filled(grid, gw, gh, gx + 1, gy))
          lf++;
        if (!grid_filled(grid, gw, gh, gx, gy - 1))
          lf++;
        if (!grid_filled(grid, gw, gh, gx, gy + 1))
          lf++;
      }

  uint32_t tri_count = (fc + lf) * 2;

  char header[80];
  memset(header, 0, sizeof(header));
  snprintf(header, sizeof(header), "QR Code STL");
  fwrite(header, 1, 80, f);
  fwrite(&tri_count, sizeof(uint32_t), 1, f);

  /* emit voxel faces */
  for (int vz = 0; vz < 2; vz++) {
    float zlo = zv[vz], zhi = zv[vz + 1];
    for (int vy = 0; vy < n; vy++)
      for (int vx = 0; vx < n; vx++) {
        if (!vfill(present, n, vx, vy, vz))
          continue;
        float x0 = (float)vx, x1 = x0 + 1;
        float y0 = (float)vy, y1 = y0 + 1;

        if (!vfill(present, n, vx, vy, vz - 1)) { /* -z */
          write_triangle_stl(f, 0, 0, -1, x0, y0, zlo, x0, y1, zlo, x1, y1,
                             zlo);
          write_triangle_stl(f, 0, 0, -1, x0, y0, zlo, x1, y1, zlo, x1, y0,
                             zlo);
        }
        if (!vfill(present, n, vx, vy, vz + 1)) { /* +z */
          write_triangle_stl(f, 0, 0, 1, x0, y0, zhi, x1, y0, zhi, x1, y1, zhi);
          write_triangle_stl(f, 0, 0, 1, x0, y0, zhi, x1, y1, zhi, x0, y1, zhi);
        }
        if (!vfill(present, n, vx - 1, vy, vz)) { /* -x */
          write_triangle_stl(f, -1, 0, 0, x0, y0, zlo, x0, y0, zhi, x0, y1,
                             zhi);
          write_triangle_stl(f, -1, 0, 0, x0, y0, zlo, x0, y1, zhi, x0, y1,
                             zlo);
        }
        if (!vfill(present, n, vx + 1, vy, vz)) { /* +x */
          write_triangle_stl(f, 1, 0, 0, x1, y0, zlo, x1, y1, zlo, x1, y1, zhi);
          write_triangle_stl(f, 1, 0, 0, x1, y0, zlo, x1, y1, zhi, x1, y0, zhi);
        }
        if (!vfill(present, n, vx, vy - 1, vz)) { /* -y */
          write_triangle_stl(f, 0, -1, 0, x0, y0, zlo, x1, y0, zlo, x1, y0,
                             zhi);
          write_triangle_stl(f, 0, -1, 0, x0, y0, zlo, x1, y0, zhi, x0, y0,
                             zhi);
        }
        if (!vfill(present, n, vx, vy + 1, vz)) { /* +y */
          write_triangle_stl(f, 0, 1, 0, x0, y1, zlo, x0, y1, zhi, x1, y1, zhi);
          write_triangle_stl(f, 0, 1, 0, x0, y1, zlo, x1, y1, zhi, x1, y1, zlo);
        }
      }
  }

  /* logo extrusion — independent closed boxes */
  if (has_logo) {
    float base_x = (float)lmx0, base_y = (float)lmy0;
    float zb = 0.5f, zt = 1.5f;
    for (int gy = 0; gy < gh; gy++)
      for (int gx = 0; gx < gw; gx++) {
        if (!grid[gy * gw + gx])
          continue;
        float fx = base_x + gx * cell, fy = base_y + gy * cell;
        float fx1 = fx + cell, fy1 = fy + cell;
        /* top */
        write_triangle_stl(f, 0, 0, 1, fx, fy, zt, fx1, fy, zt, fx1, fy1, zt);
        write_triangle_stl(f, 0, 0, 1, fx, fy, zt, fx1, fy1, zt, fx, fy1, zt);
        /* bottom */
        write_triangle_stl(f, 0, 0, -1, fx, fy, zb, fx, fy1, zb, fx1, fy1, zb);
        write_triangle_stl(f, 0, 0, -1, fx, fy, zb, fx1, fy1, zb, fx1, fy, zb);
        if (!grid_filled(grid, gw, gh, gx - 1, gy)) {
          write_triangle_stl(f, -1, 0, 0, fx, fy, zb, fx, fy, zt, fx, fy1, zt);
          write_triangle_stl(f, -1, 0, 0, fx, fy, zb, fx, fy1, zt, fx, fy1, zb);
        }
        if (!grid_filled(grid, gw, gh, gx + 1, gy)) {
          write_triangle_stl(f, 1, 0, 0, fx1, fy, zb, fx1, fy1, zb, fx1, fy1,
                             zt);
          write_triangle_stl(f, 1, 0, 0, fx1, fy, zb, fx1, fy1, zt, fx1, fy,
                             zt);
        }
        if (!grid_filled(grid, gw, gh, gx, gy - 1)) {
          write_triangle_stl(f, 0, -1, 0, fx, fy, zb, fx1, fy, zb, fx1, fy, zt);
          write_triangle_stl(f, 0, -1, 0, fx, fy, zb, fx1, fy, zt, fx, fy, zt);
        }
        if (!grid_filled(grid, gw, gh, gx, gy + 1)) {
          write_triangle_stl(f, 0, 1, 0, fx, fy1, zb, fx, fy1, zt, fx1, fy1,
                             zt);
          write_triangle_stl(f, 0, 1, 0, fx, fy1, zb, fx1, fy1, zt, fx1, fy1,
                             zb);
        }
      }
  }

  free(present);
  free(grid);
  fclose(f);
  return true;
}

/* ── OBJ export (voxel-based, shared vertex grid) ────────────────── */

static bool export_obj(const uint8_t *qr, int qr_size, const char *path,
                       const char *logo_path) {
  FILE *f = fopen(path, "w");
  if (!f)
    return false;
  fprintf(f, "# QR Code OBJ\n");
  int n = qr_size;
  int nv = n + 1;

  bool *grid;
  int gw, gh, lmx0, lmy0, lmw, lmh;
  float cell;
  bool has_logo;
  setup_logo(logo_path, n, &grid, &gw, &gh, &cell, &lmx0, &lmy0, &lmw, &lmh,
             &has_logo);

  bool *present = build_present_map(qr, n, has_logo, lmx0, lmy0, lmw, lmh);
  if (!present) {
    free(grid);
    fclose(f);
    return false;
  }

  /* shared vertex grid: 3 z-levels × (n+1)² */
  float zv[3] = {0.0f, 0.5f, 1.5f};
  for (int z = 0; z < 3; z++)
    for (int y = 0; y <= n; y++)
      for (int x = 0; x <= n; x++)
        fprintf(f, "v %d %d %f\n", x, y, zv[z]);

/* vertex index macro (1-based) */
#define V(z, vx, vy) (1 + (z) * nv * nv + (vy) * nv + (vx))

  /* emit voxel faces as quads referencing shared vertices */
  for (int vz = 0; vz < 2; vz++) {
    int zlo = vz, zhi = vz + 1;
    for (int vy = 0; vy < n; vy++)
      for (int vx = 0; vx < n; vx++) {
        if (!vfill(present, n, vx, vy, vz))
          continue;

        if (!vfill(present, n, vx, vy, vz - 1)) /* -z */
          fprintf(f, "f %d %d %d %d\n", V(zlo, vx, vy), V(zlo, vx, vy + 1),
                  V(zlo, vx + 1, vy + 1), V(zlo, vx + 1, vy));
        if (!vfill(present, n, vx, vy, vz + 1)) /* +z */
          fprintf(f, "f %d %d %d %d\n", V(zhi, vx, vy), V(zhi, vx + 1, vy),
                  V(zhi, vx + 1, vy + 1), V(zhi, vx, vy + 1));
        if (!vfill(present, n, vx - 1, vy, vz)) /* -x */
          fprintf(f, "f %d %d %d %d\n", V(zlo, vx, vy), V(zhi, vx, vy),
                  V(zhi, vx, vy + 1), V(zlo, vx, vy + 1));
        if (!vfill(present, n, vx + 1, vy, vz)) /* +x */
          fprintf(f, "f %d %d %d %d\n", V(zlo, vx + 1, vy),
                  V(zlo, vx + 1, vy + 1), V(zhi, vx + 1, vy + 1),
                  V(zhi, vx + 1, vy));
        if (!vfill(present, n, vx, vy - 1, vz)) /* -y */
          fprintf(f, "f %d %d %d %d\n", V(zlo, vx, vy), V(zlo, vx + 1, vy),
                  V(zhi, vx + 1, vy), V(zhi, vx, vy));
        if (!vfill(present, n, vx, vy + 1, vz)) /* +y */
          fprintf(f, "f %d %d %d %d\n", V(zlo, vx, vy + 1), V(zhi, vx, vy + 1),
                  V(zhi, vx + 1, vy + 1), V(zlo, vx + 1, vy + 1));
      }
  }
#undef V

  /* logo extrusion — independent closed boxes (separate body) */
  if (has_logo) {
    int vi = 3 * nv * nv; /* continue vertex numbering */
    float base_x = (float)lmx0, base_y = (float)lmy0;
    float zb = 0.5f, zt = 1.5f;
    for (int gy = 0; gy < gh; gy++)
      for (int gx = 0; gx < gw; gx++) {
        if (!grid[gy * gw + gx])
          continue;
        float fx = base_x + gx * cell, fy = base_y + gy * cell;
        float fx1 = fx + cell, fy1 = fy + cell;
        write_quad_obj(f, fx, fy, zt, fx1, fy, zt, fx1, fy1, zt, fx, fy1, zt,
                       &vi);
        write_quad_obj(f, fx, fy, zb, fx, fy1, zb, fx1, fy1, zb, fx1, fy, zb,
                       &vi);
        if (!grid_filled(grid, gw, gh, gx - 1, gy))
          write_quad_obj(f, fx, fy, zb, fx, fy, zt, fx, fy1, zt, fx, fy1, zb,
                         &vi);
        if (!grid_filled(grid, gw, gh, gx + 1, gy))
          write_quad_obj(f, fx1, fy1, zb, fx1, fy1, zt, fx1, fy, zt, fx1, fy,
                         zb, &vi);
        if (!grid_filled(grid, gw, gh, gx, gy - 1))
          write_quad_obj(f, fx1, fy, zb, fx1, fy, zt, fx, fy, zt, fx, fy, zb,
                         &vi);
        if (!grid_filled(grid, gw, gh, gx, gy + 1))
          write_quad_obj(f, fx, fy1, zb, fx, fy1, zt, fx1, fy1, zt, fx1, fy1,
                         zb, &vi);
      }
  }

  free(present);
  free(grid);
  fclose(f);
  return true;
}

/* ── Main ────────────────────────────────────────────────────────── */

int main(void) {
  char data[1024] = {0};
  char logo_path[512] = {0};
  char svg_tmp[512] = {0}; /* temp PNG if SVG was converted */
  char basename[256] = {0};
  int type = 0;
  int fmt = 0;

  /* init ncurses */
  initscr();
  noecho();
  curs_set(0);
  keypad(stdscr, TRUE);

  /* ── Screen 1: Type selection ──────────────────────────────── */
  tui_title("QR Code Generator — Select Type");
  mvprintw(3, 4, "1) URL");
  mvprintw(4, 4, "2) Plain Text");
  mvprintw(5, 4, "3) WiFi Network");
  mvprintw(7, 4, "Press 1, 2, or 3:");
  refresh();

  while (1) {
    int ch = getch();
    if (ch == '1') {
      type = 1;
      break;
    }
    if (ch == '2') {
      type = 2;
      break;
    }
    if (ch == '3') {
      type = 3;
      break;
    }
    if (ch == 'q' || ch == 27) {
      endwin();
      return 0;
    }
  }

  /* ── Screen 2: Data input ──────────────────────────────────── */
  if (type == 1) {
    tui_title("Enter URL");
    tui_prompt(3, "URL", data, sizeof(data));
  } else if (type == 2) {
    tui_title("Enter Text");
    tui_prompt(3, "Text", data, sizeof(data));
  } else {
    char ssid[256] = {0};
    char pass[256] = {0};
    char enc[16] = {0};

    tui_title("WiFi Network Details");
    tui_prompt(3, "SSID", ssid, sizeof(ssid));
    tui_prompt(4, "Password", pass, sizeof(pass));
    mvprintw(6, 4, "Encryption: 1) WPA  2) WEP  3) None");
    mvprintw(7, 4, "Press 1, 2, or 3:");
    refresh();
    noecho();
    curs_set(0);

    while (1) {
      int ch = getch();
      if (ch == '1') {
        strcpy(enc, "WPA");
        break;
      }
      if (ch == '2') {
        strcpy(enc, "WEP");
        break;
      }
      if (ch == '3') {
        strcpy(enc, "nopass");
        break;
      }
    }

    snprintf(data, sizeof(data), "WIFI:T:%s;S:%s;P:%s;;", enc, ssid, pass);
  }

  /* ── Screen 3: Logo option ─────────────────────────────────── */
  tui_title("Logo Overlay");
  mvprintw(3, 4, "Add a logo to the center of the QR code?");
  mvprintw(4, 4, "(PNG: overlay, STL/OBJ: embossed extrusion)");
  mvprintw(6, 4, "y) Yes  n) No");
  refresh();

  while (1) {
    int ch = getch();
    if (ch == 'y' || ch == 'Y') {
      tui_prompt(8, "Logo image path", logo_path, sizeof(logo_path));
      if (access(logo_path, R_OK) != 0) {
        mvprintw(10, 4, "File not found! Continuing without logo.");
        logo_path[0] = '\0';
        refresh();
        napms(1500);
      } else if (has_svg_ext(logo_path)) {
        mvprintw(10, 4, "Converting SVG to PNG...");
        refresh();
        if (convert_svg_to_png(logo_path, svg_tmp, sizeof(svg_tmp))) {
          snprintf(logo_path, sizeof(logo_path), "%s", svg_tmp);
          mvprintw(11, 4, "SVG converted successfully.");
        } else {
          mvprintw(11, 4, "SVG conversion failed! Continuing without logo.");
          logo_path[0] = '\0';
          svg_tmp[0] = '\0';
        }
        refresh();
        napms(1000);
      }
      break;
    }
    if (ch == 'n' || ch == 'N') {
      break;
    }
  }

  /* ── Screen 4: Export format ────────────────────────────────── */
  tui_title("Export Format");
  mvprintw(3, 4, "1) PNG");
  mvprintw(4, 4, "2) STL (3D print)");
  mvprintw(5, 4, "3) OBJ (3D model)");
  mvprintw(6, 4, "4) All formats");
  mvprintw(8, 4, "Press 1, 2, 3, or 4:");
  refresh();

  while (1) {
    int ch = getch();
    if (ch == '1') {
      fmt = FMT_PNG;
      break;
    }
    if (ch == '2') {
      fmt = FMT_STL;
      break;
    }
    if (ch == '3') {
      fmt = FMT_OBJ;
      break;
    }
    if (ch == '4') {
      fmt = FMT_ALL;
      break;
    }
  }

  /* ── Screen 5: Output filename ─────────────────────────────── */
  tui_title("Output Filename");
  mvprintw(3, 4, "Extensions will be added automatically.");
  tui_prompt(5, "Base name (e.g. qrcode)", basename, sizeof(basename));

  if (basename[0] == '\0')
    strcpy(basename, "qrcode");

  /* done with TUI */
  endwin();

  /* ── Generate QR code ──────────────────────────────────────── */
  uint8_t qr[qrcodegen_BUFFER_LEN_MAX];
  uint8_t tmp[qrcodegen_BUFFER_LEN_MAX];

  bool ok = qrcodegen_encodeText(data, tmp, qr, qrcodegen_Ecc_HIGH,
                                 qrcodegen_VERSION_MIN, qrcodegen_VERSION_MAX,
                                 qrcodegen_Mask_AUTO, true);

  if (!ok) {
    fprintf(stderr, "Error: QR code encoding failed.\n");
    return 1;
  }

  int qr_size = qrcodegen_getSize(qr);
  printf("QR code generated (%dx%d modules)\n", qr_size, qr_size);

  /* ── Export files ──────────────────────────────────────────── */
  char filename[512];

  if (fmt & FMT_PNG) {
    snprintf(filename, sizeof(filename), "%s.png", basename);
    if (export_png(qr, qr_size, filename, logo_path))
      printf("Exported: %s\n", filename);
    else
      fprintf(stderr, "Error writing %s\n", filename);
  }

  if (fmt & FMT_STL) {
    snprintf(filename, sizeof(filename), "%s.stl", basename);
    if (export_stl(qr, qr_size, filename, logo_path))
      printf("Exported: %s\n", filename);
    else
      fprintf(stderr, "Error writing %s\n", filename);
  }

  if (fmt & FMT_OBJ) {
    snprintf(filename, sizeof(filename), "%s.obj", basename);
    if (export_obj(qr, qr_size, filename, logo_path))
      printf("Exported: %s\n", filename);
    else
      fprintf(stderr, "Error writing %s\n", filename);
  }

  /* clean up temp SVG conversion */
  if (svg_tmp[0])
    unlink(svg_tmp);

  return 0;
}

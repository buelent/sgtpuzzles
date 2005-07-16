/*
 * untangle.c: Game about planar graphs. You are given a graph
 * represented by points and straight lines, with some lines
 * crossing; your task is to drag the points into a configuration
 * where none of the lines cross.
 * 
 * Cloned from a Flash game called `Planarity', by John Tantalo.
 * <http://home.cwru.edu/~jnt5/Planarity> at the time of writing
 * this. The Flash game had a fixed set of levels; my added value,
 * as usual, is automatic generation of random games to order.
 */

/*
 * TODO:
 * 
 *  - Docs and checklist etc
 *  - Any way we can speed up redraws on GTK? Uck.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>
#include <ctype.h>
#include <math.h>

#include "puzzles.h"
#include "tree234.h"

#define CIRCLE_RADIUS 6
#define DRAG_THRESHOLD (CIRCLE_RADIUS * 2)
#define PREFERRED_TILESIZE 64

#define FLASH_TIME 0.13F
#define ANIM_TIME 0.13F
#define SOLVEANIM_TIME 0.50F

enum {
    COL_BACKGROUND,
    COL_LINE,
    COL_OUTLINE,
    COL_POINT,
    COL_DRAGPOINT,
    COL_NEIGHBOUR,
    NCOLOURS
};

typedef struct point {
    /*
     * Points are stored using rational coordinates, with the same
     * denominator for both coordinates.
     */
    int x, y, d;
} point;

typedef struct edge {
    /*
     * This structure is implicitly associated with a particular
     * point set, so all it has to do is to store two point
     * indices. It is required to store them in the order (lower,
     * higher), i.e. a < b always.
     */
    int a, b;
} edge;

struct game_params {
    int n;			       /* number of points */
};

struct graph {
    int refcount;		       /* for deallocation */
    tree234 *edges;		       /* stores `edge' structures */
};

struct game_state {
    game_params params;
    int w, h;			       /* extent of coordinate system only */
    point *pts;
    struct graph *graph;
    int completed, cheated, just_solved;
};

static int edgecmpC(const void *av, const void *bv)
{
    const edge *a = (const edge *)av;
    const edge *b = (const edge *)bv;

    if (a->a < b->a)
	return -1;
    else if (a->a > b->a)
	return +1;
    else if (a->b < b->b)
	return -1;
    else if (a->b > b->b)
	return +1;
    return 0;
}

static int edgecmp(void *av, void *bv) { return edgecmpC(av, bv); }

static game_params *default_params(void)
{
    game_params *ret = snew(game_params);

    ret->n = 10;

    return ret;
}

static int game_fetch_preset(int i, char **name, game_params **params)
{
    game_params *ret;
    int n;
    char buf[80];

    switch (i) {
      case 0: n = 6; break;
      case 1: n = 10; break;
      case 2: n = 15; break;
      case 3: n = 20; break;
      case 4: n = 25; break;
      default: return FALSE;
    }

    sprintf(buf, "%d points", n);
    *name = dupstr(buf);

    *params = ret = snew(game_params);
    ret->n = n;

    return TRUE;
}

static void free_params(game_params *params)
{
    sfree(params);
}

static game_params *dup_params(game_params *params)
{
    game_params *ret = snew(game_params);
    *ret = *params;		       /* structure copy */
    return ret;
}

static void decode_params(game_params *params, char const *string)
{
    params->n = atoi(string);
}

static char *encode_params(game_params *params, int full)
{
    char buf[80];

    sprintf(buf, "%d", params->n);

    return dupstr(buf);
}

static config_item *game_configure(game_params *params)
{
    config_item *ret;
    char buf[80];

    ret = snewn(3, config_item);

    ret[0].name = "Number of points";
    ret[0].type = C_STRING;
    sprintf(buf, "%d", params->n);
    ret[0].sval = dupstr(buf);
    ret[0].ival = 0;

    ret[1].name = NULL;
    ret[1].type = C_END;
    ret[1].sval = NULL;
    ret[1].ival = 0;

    return ret;
}

static game_params *custom_params(config_item *cfg)
{
    game_params *ret = snew(game_params);

    ret->n = atoi(cfg[0].sval);

    return ret;
}

static char *validate_params(game_params *params, int full)
{
    if (params->n < 4)
        return "Number of points must be at least four";
    return NULL;
}

/*
 * Determine whether the line segments between a1 and a2, and
 * between b1 and b2, intersect. We count it as an intersection if
 * any of the endpoints lies _on_ the other line.
 */
static int cross(point a1, point a2, point b1, point b2)
{
    int b1x, b1y, b2x, b2y, px, py, d1, d2, d3;

    /*
     * The condition for crossing is that b1 and b2 are on opposite
     * sides of the line a1-a2, and vice versa. We determine this
     * by taking the dot product of b1-a1 with a vector
     * perpendicular to a2-a1, and similarly with b2-a1, and seeing
     * if they have different signs.
     */

    /*
     * Construct the vector b1-a1. We don't have to worry too much
     * about the denominator, because we're only going to check the
     * sign of this vector; we just need to get the numerator
     * right.
     */
    b1x = b1.x * a1.d - a1.x * b1.d;
    b1y = b1.y * a1.d - a1.y * b1.d;
    /* Now construct b2-a1, and a vector perpendicular to a2-a1,
     * in the same way. */
    b2x = b2.x * a1.d - a1.x * b2.d;
    b2y = b2.y * a1.d - a1.y * b2.d;
    px = a1.y * a2.d - a2.y * a1.d;
    py = a2.x * a1.d - a1.x * a2.d;
    /* Take the dot products. */
    d1 = b1x * px + b1y * py;
    d2 = b2x * px + b2y * py;
    /* If they have the same non-zero sign, the lines do not cross. */
    if ((d1 > 0 && d2 > 0) || (d1 < 0 && d2 < 0))
	return FALSE;

    /*
     * If the dot products are both exactly zero, then the two line
     * segments are collinear. At this point the intersection
     * condition becomes whether or not they overlap within their
     * line.
     */
    if (d1 == 0 && d2 == 0) {
	/* Construct the vector a2-a1. */
	px = a2.x * a1.d - a1.x * a2.d;
	py = a2.y * a1.d - a1.y * a2.d;
	/* Determine the dot products of b1-a1 and b2-a1 with this. */
	d1 = b1x * px + b1y * py;
	d2 = b2x * px + b2y * py;
	/* If they're both strictly negative, the lines do not cross. */
	if (d1 < 0 && d2 < 0)
	    return FALSE;
	/* Otherwise, take the dot product of a2-a1 with itself. If
	 * the other two dot products both exceed this, the lines do
	 * not cross. */
	d3 = px * px + py * py;
	if (d1 > d3 && d2 > d3)
	    return FALSE;
    }

    /*
     * We've eliminated the only important special case, and we
     * have determined that b1 and b2 are on opposite sides of the
     * line a1-a2. Now do the same thing the other way round and
     * we're done.
     */
    b1x = a1.x * b1.d - b1.x * a1.d;
    b1y = a1.y * b1.d - b1.y * a1.d;
    b2x = a2.x * b1.d - b1.x * a2.d;
    b2y = a2.y * b1.d - b1.y * a2.d;
    px = b1.y * b2.d - b2.y * b1.d;
    py = b2.x * b1.d - b1.x * b2.d;
    d1 = b1x * px + b1y * py;
    d2 = b2x * px + b2y * py;
    if ((d1 > 0 && d2 > 0) || (d1 < 0 && d2 < 0))
	return FALSE;

    /*
     * The lines must cross.
     */
    return TRUE;
}

static unsigned long squarert(unsigned long n) {
    unsigned long d, a, b, di;

    d = n;
    a = 0;
    b = 1 << 30;		       /* largest available power of 4 */
    do {
        a >>= 1;
        di = 2*a + b;
        if (di <= d) {
            d -= di;
            a += b;
        }
        b >>= 2;
    } while (b);

    return a;
}

/*
 * Our solutions are arranged on a square grid big enough that n
 * points occupy about 1/POINTDENSITY of the grid.
 */
#define POINTDENSITY 3
#define MAXDEGREE 4
#define COORDLIMIT(n) squarert((n) * POINTDENSITY)

static void addedge(tree234 *edges, int a, int b)
{
    edge *e = snew(edge);

    assert(a != b);

    e->a = min(a, b);
    e->b = max(a, b);

    add234(edges, e);
}

static int isedge(tree234 *edges, int a, int b)
{
    edge e;

    assert(a != b);

    e.a = min(a, b);
    e.b = max(a, b);

    return find234(edges, &e, NULL) != NULL;
}

typedef struct vertex {
    int param;
    int vindex;
} vertex;

static int vertcmpC(const void *av, const void *bv)
{
    const vertex *a = (vertex *)av;
    const vertex *b = (vertex *)bv;

    if (a->param < b->param)
	return -1;
    else if (a->param > b->param)
	return +1;
    else if (a->vindex < b->vindex)
	return -1;
    else if (a->vindex > b->vindex)
	return +1;
    return 0;
}
static int vertcmp(void *av, void *bv) { return vertcmpC(av, bv); }

/*
 * Construct point coordinates for n points arranged in a circle,
 * within the bounding box (0,0) to (w,w).
 */
static void make_circle(point *pts, int n, int w)
{
    int d, r, c, i;

    /*
     * First, decide on a denominator. Although in principle it
     * would be nice to set this really high so as to finely
     * distinguish all the points on the circle, I'm going to set
     * it at a fixed size to prevent integer overflow problems.
     */
    d = PREFERRED_TILESIZE;

    /*
     * Leave a little space outside the circle.
     */
    c = d * w / 2;
    r = d * w * 3 / 7;

    /*
     * Place the points.
     */
    for (i = 0; i < n; i++) {
	double angle = i * 2 * PI / n;
	double x = r * sin(angle), y = - r * cos(angle);
	pts[i].x = (int)(c + x + 0.5);
	pts[i].y = (int)(c + y + 0.5);
	pts[i].d = d;
    }
}

static char *new_game_desc(game_params *params, random_state *rs,
			   char **aux, int interactive)
{
    int n = params->n;
    int w, h, i, j, k, m;
    point *pts, *pts2;
    int *tmp;
    tree234 *edges, *vertices;
    edge *e, *e2;
    vertex *v, *vs, *vlist;
    char *ret;

    w = h = COORDLIMIT(n);

    /*
     * Choose n points from this grid.
     */
    pts = snewn(n, point);
    tmp = snewn(w*h, int);
    for (i = 0; i < w*h; i++)
	tmp[i] = i;
    shuffle(tmp, w*h, sizeof(*tmp), rs);
    for (i = 0; i < n; i++) {
	pts[i].x = tmp[i] % w;
	pts[i].y = tmp[i] / w;
	pts[i].d = 1;
    }
    sfree(tmp);

    /*
     * Now start adding edges between the points.
     * 
     * At all times, we attempt to add an edge to the lowest-degree
     * vertex we currently have, and we try the other vertices as
     * candidate second endpoints in order of distance from this
     * one. We stop as soon as we find an edge which
     * 
     *  (a) does not increase any vertex's degree beyond MAXDEGREE
     *  (b) does not cross any existing edges
     *  (c) does not intersect any actual point.
     */
    vs = snewn(n, vertex);
    vertices = newtree234(vertcmp);
    for (i = 0; i < n; i++) {
	v = vs + i;
	v->param = 0;		       /* in this tree, param is the degree */
	v->vindex = i;
	add234(vertices, v);
    }
    edges = newtree234(edgecmp);
    vlist = snewn(n, vertex);
    while (1) {
	int added = FALSE;

	for (i = 0; i < n; i++) {
	    v = index234(vertices, i);
	    j = v->vindex;

	    if (v->param >= MAXDEGREE)
		break;		       /* nothing left to add! */

	    /*
	     * Sort the other vertices into order of their distance
	     * from this one. Don't bother looking below i, because
	     * we've already tried those edges the other way round.
	     * Also here we rule out target vertices with too high
	     * a degree, and (of course) ones to which we already
	     * have an edge.
	     */
	    m = 0;
	    for (k = i+1; k < n; k++) {
		vertex *kv = index234(vertices, k);
		int ki = kv->vindex;
		int dx, dy;

		if (kv->param >= MAXDEGREE || isedge(edges, ki, j))
		    continue;

		vlist[m].vindex = ki;
		dx = pts[ki].x - pts[j].x;
		dy = pts[ki].y - pts[j].y;
		vlist[m].param = dx*dx + dy*dy;
		m++;
	    }

	    qsort(vlist, m, sizeof(*vlist), vertcmpC);

	    for (k = 0; k < m; k++) {
		int p;
		int ki = vlist[k].vindex;

		/*
		 * Check to see whether this edge intersects any
		 * existing edge or point.
		 */
		for (p = 0; p < n; p++)
		    if (p != ki && p != j && cross(pts[ki], pts[j],
						   pts[p], pts[p]))
			break;
		if (p < n)
		    continue;
		for (p = 0; (e = index234(edges, p)) != NULL; p++)
		    if (e->a != ki && e->a != j &&
			e->b != ki && e->b != j &&
			cross(pts[ki], pts[j], pts[e->a], pts[e->b]))
			break;
		if (e)
		    continue;

		/*
		 * We're done! Add this edge, modify the degrees of
		 * the two vertices involved, and break.
		 */
		addedge(edges, j, ki);
		added = TRUE;
		del234(vertices, vs+j);
		vs[j].param++;
		add234(vertices, vs+j);
		del234(vertices, vs+ki);
		vs[ki].param++;
		add234(vertices, vs+ki);
		break;
	    }

	    if (k < m)
		break;
	}

	if (!added)
	    break;		       /* we're done. */
    }

    /*
     * That's our graph. Now shuffle the points, making sure that
     * they come out with at least one crossed line when arranged
     * in a circle (so that the puzzle isn't immediately solved!).
     */
    tmp = snewn(n, int);
    for (i = 0; i < n; i++)
	tmp[i] = i;
    pts2 = snewn(n, point);
    make_circle(pts2, n, w);
    while (1) {
	shuffle(tmp, n, sizeof(*tmp), rs);
	for (i = 0; (e = index234(edges, i)) != NULL; i++) {
	    for (j = i+1; (e2 = index234(edges, j)) != NULL; j++) {
		if (e2->a == e->a || e2->a == e->b ||
		    e2->b == e->a || e2->b == e->b)
		    continue;
		if (cross(pts2[tmp[e2->a]], pts2[tmp[e2->b]],
			  pts2[tmp[e->a]], pts2[tmp[e->b]]))
		    break;
	    }
	    if (e2)
		break;
	}
	if (e)
	    break;		       /* we've found a crossing */
    }

    /*
     * We're done. Now encode the graph in a string format. Let's
     * use a comma-separated list of dash-separated vertex number
     * pairs, numbered from zero. We'll sort the list to prevent
     * side channels.
     */
    ret = NULL;
    {
	char *sep;
	char buf[80];
	int retlen;
	edge *ea;

	retlen = 0;
	m = count234(edges);
	ea = snewn(m, edge);
	for (i = 0; (e = index234(edges, i)) != NULL; i++) {
	    assert(i < m);
	    ea[i].a = min(tmp[e->a], tmp[e->b]);
	    ea[i].b = max(tmp[e->a], tmp[e->b]);
	    retlen += 1 + sprintf(buf, "%d-%d", ea[i].a, ea[i].b);
	}
	assert(i == m);
	qsort(ea, m, sizeof(*ea), edgecmpC);

	ret = snewn(retlen, char);
	sep = "";
	k = 0;

	for (i = 0; i < m; i++) {
	    k += sprintf(ret + k, "%s%d-%d", sep, ea[i].a, ea[i].b);
	    sep = ",";
	}
	assert(k < retlen);

	sfree(ea);
    }

    /*
     * Encode the solution we started with as an aux_info string.
     */
    {
	char buf[80];
	char *auxstr;
	int auxlen;

	auxlen = 2;		       /* leading 'S' and trailing '\0' */
	for (i = 0; i < n; i++) {
	    j = tmp[i];
	    pts2[j] = pts[i];
	    if (pts2[j].d & 1) {
		pts2[j].x *= 2;
		pts2[j].y *= 2;
		pts2[j].d *= 2;
	    }
	    pts2[j].x += pts2[j].d / 2;
	    pts2[j].y += pts2[j].d / 2;
	    auxlen += sprintf(buf, ";P%d:%d,%d/%d", i,
			      pts2[j].x, pts2[j].y, pts2[j].d);
	}
	k = 0;
	auxstr = snewn(auxlen, char);
	auxstr[k++] = 'S';
	for (i = 0; i < n; i++)
	    k += sprintf(auxstr+k, ";P%d:%d,%d/%d", i,
			 pts2[i].x, pts2[i].y, pts2[i].d);
	assert(k < auxlen);
	*aux = auxstr;
    }
    sfree(pts2);

    sfree(tmp);
    sfree(vlist);
    freetree234(vertices);
    sfree(vs);
    while ((e = delpos234(edges, 0)) != NULL)
	sfree(e);
    freetree234(edges);
    sfree(pts);

    return ret;
}

static char *validate_desc(game_params *params, char *desc)
{
    int a, b;

    while (*desc) {
	a = atoi(desc);
	if (a < 0 || a >= params->n)
	    return "Number out of range in game description";
	while (*desc && isdigit((unsigned char)*desc)) desc++;
	if (*desc != '-')
	    return "Expected '-' after number in game description";
	desc++;			       /* eat dash */
	b = atoi(desc);
	if (b < 0 || b >= params->n)
	    return "Number out of range in game description";
	while (*desc && isdigit((unsigned char)*desc)) desc++;
	if (*desc) {
	    if (*desc != ',')
		return "Expected ',' after number in game description";
	    desc++;		       /* eat comma */
	}
    }

    return NULL;
}

static game_state *new_game(midend_data *me, game_params *params, char *desc)
{
    int n = params->n;
    game_state *state = snew(game_state);
    int a, b;

    state->params = *params;
    state->w = state->h = COORDLIMIT(n);
    state->pts = snewn(n, point);
    make_circle(state->pts, n, state->w);
    state->graph = snew(struct graph);
    state->graph->refcount = 1;
    state->graph->edges = newtree234(edgecmp);
    state->completed = state->cheated = state->just_solved = FALSE;

    while (*desc) {
	a = atoi(desc);
	assert(a >= 0 && a < params->n);
	while (*desc && isdigit((unsigned char)*desc)) desc++;
	assert(*desc == '-');
	desc++;			       /* eat dash */
	b = atoi(desc);
	assert(b >= 0 && b < params->n);
	while (*desc && isdigit((unsigned char)*desc)) desc++;
	if (*desc) {
	    assert(*desc == ',');
	    desc++;		       /* eat comma */
	}
	addedge(state->graph->edges, a, b);
    }

    return state;
}

static game_state *dup_game(game_state *state)
{
    int n = state->params.n;
    game_state *ret = snew(game_state);

    ret->params = state->params;
    ret->w = state->w;
    ret->h = state->h;
    ret->pts = snewn(n, point);
    memcpy(ret->pts, state->pts, n * sizeof(point));
    ret->graph = state->graph;
    ret->graph->refcount++;
    ret->completed = state->completed;
    ret->cheated = state->cheated;
    ret->just_solved = state->just_solved;

    return ret;
}

static void free_game(game_state *state)
{
    if (--state->graph->refcount <= 0) {
	edge *e;
	while ((e = delpos234(state->graph->edges, 0)) != NULL)
	    sfree(e);
	freetree234(state->graph->edges);
	sfree(state->graph);
    }
    sfree(state->pts);
    sfree(state);
}

static char *solve_game(game_state *state, game_state *currstate,
			char *aux, char **error)
{
    if (!aux) {
	*error = "Solution not known for this puzzle";
	return NULL;
    }

    return dupstr(aux);
}

static char *game_text_format(game_state *state)
{
    return NULL;
}

struct game_ui {
    int dragpoint;		       /* point being dragged; -1 if none */
    point newpoint;		       /* where it's been dragged to so far */
    int just_dragged;		       /* reset in game_changed_state */
    int just_moved;		       /* _set_ in game_changed_state */
    float anim_length;
};

static game_ui *new_ui(game_state *state)
{
    game_ui *ui = snew(game_ui);
    ui->dragpoint = -1;
    ui->just_moved = ui->just_dragged = FALSE;
    return ui;
}

static void free_ui(game_ui *ui)
{
    sfree(ui);
}

static char *encode_ui(game_ui *ui)
{
    return NULL;
}

static void decode_ui(game_ui *ui, char *encoding)
{
}

static void game_changed_state(game_ui *ui, game_state *oldstate,
                               game_state *newstate)
{
    ui->dragpoint = -1;
    ui->just_moved = ui->just_dragged;
    ui->just_dragged = FALSE;
}

struct game_drawstate {
    int tilesize;
};

static char *interpret_move(game_state *state, game_ui *ui, game_drawstate *ds,
			    int x, int y, int button)
{
    int n = state->params.n;

    if (button == LEFT_BUTTON) {
	int i, best, bestd;

	/*
	 * Begin drag. We drag the vertex _nearest_ to the pointer,
	 * just in case one is nearly on top of another and we want
	 * to drag the latter. However, we drag nothing at all if
	 * the nearest vertex is outside DRAG_THRESHOLD.
	 */
	best = -1;
	bestd = 0;

	for (i = 0; i < n; i++) {
	    int px = state->pts[i].x * ds->tilesize / state->pts[i].d;
	    int py = state->pts[i].y * ds->tilesize / state->pts[i].d;
	    int dx = px - x;
	    int dy = py - y;
	    int d = dx*dx + dy*dy;

	    if (best == -1 || bestd > d) {
		best = i;
		bestd = d;
	    }
	}

	if (bestd <= DRAG_THRESHOLD * DRAG_THRESHOLD) {
	    ui->dragpoint = best;
	    ui->newpoint.x = x;
	    ui->newpoint.y = y;
	    ui->newpoint.d = ds->tilesize;
	    return "";
	}

    } else if (button == LEFT_DRAG && ui->dragpoint >= 0) {
	ui->newpoint.x = x;
	ui->newpoint.y = y;
	ui->newpoint.d = ds->tilesize;
	return "";
    } else if (button == LEFT_RELEASE && ui->dragpoint >= 0) {
	int p = ui->dragpoint;
	char buf[80];

	ui->dragpoint = -1;	       /* terminate drag, no matter what */

	/*
	 * First, see if we're within range. The user can cancel a
	 * drag by dragging the point right off the window.
	 */
	if (ui->newpoint.x < 0 || ui->newpoint.x >= state->w*ui->newpoint.d ||
	    ui->newpoint.y < 0 || ui->newpoint.y >= state->h*ui->newpoint.d)
	    return "";

	/*
	 * We aren't cancelling the drag. Construct a move string
	 * indicating where this point is going to.
	 */
	sprintf(buf, "P%d:%d,%d/%d", p,
		ui->newpoint.x, ui->newpoint.y, ui->newpoint.d);
	ui->just_dragged = TRUE;
	return dupstr(buf);
    }

    return NULL;
}

static game_state *execute_move(game_state *state, char *move)
{
    int n = state->params.n;
    int p, x, y, d, k;
    game_state *ret = dup_game(state);

    ret->just_solved = FALSE;

    while (*move) {
	if (*move == 'S') {
	    move++;
	    if (*move == ';') move++;
	    ret->cheated = ret->just_solved = TRUE;
	}
	if (*move == 'P' &&
	    sscanf(move+1, "%d:%d,%d/%d%n", &p, &x, &y, &d, &k) == 4 &&
	    p >= 0 && p < n && d > 0) {
	    ret->pts[p].x = x;
	    ret->pts[p].y = y;
	    ret->pts[p].d = d;

	    move += k+1;
	    if (*move == ';') move++;
	} else {
	    free_game(ret);
	    return NULL;
	}
    }

    /*
     * Check correctness: for every pair of edges, see whether they
     * cross.
     */
    if (!ret->completed) {
	int i, j;
	edge *e, *e2;

	for (i = 0; (e = index234(ret->graph->edges, i)) != NULL; i++) {
	    for (j = i+1; (e2 = index234(ret->graph->edges, j)) != NULL; j++) {
		if (e2->a == e->a || e2->a == e->b ||
		    e2->b == e->a || e2->b == e->b)
		    continue;
		if (cross(ret->pts[e2->a], ret->pts[e2->b],
			  ret->pts[e->a], ret->pts[e->b]))
		    break;
	    }
	    if (e2)
		break;
	}

	/*
	 * e == NULL if we've gone through all the edge pairs
	 * without finding a crossing.
	 */
	ret->completed = (e == NULL);
    }

    return ret;
}

/* ----------------------------------------------------------------------
 * Drawing routines.
 */

static void game_compute_size(game_params *params, int tilesize,
			      int *x, int *y)
{
    *x = *y = COORDLIMIT(params->n) * tilesize;
}

static void game_set_size(game_drawstate *ds, game_params *params,
			  int tilesize)
{
    ds->tilesize = tilesize;
}

static float *game_colours(frontend *fe, game_state *state, int *ncolours)
{
    float *ret = snewn(3 * NCOLOURS, float);

    frontend_default_colour(fe, &ret[COL_BACKGROUND * 3]);

    ret[COL_LINE * 3 + 0] = 0.0F;
    ret[COL_LINE * 3 + 1] = 0.0F;
    ret[COL_LINE * 3 + 2] = 0.0F;

    ret[COL_OUTLINE * 3 + 0] = 0.0F;
    ret[COL_OUTLINE * 3 + 1] = 0.0F;
    ret[COL_OUTLINE * 3 + 2] = 0.0F;

    ret[COL_POINT * 3 + 0] = 0.0F;
    ret[COL_POINT * 3 + 1] = 0.0F;
    ret[COL_POINT * 3 + 2] = 1.0F;

    ret[COL_DRAGPOINT * 3 + 0] = 1.0F;
    ret[COL_DRAGPOINT * 3 + 1] = 1.0F;
    ret[COL_DRAGPOINT * 3 + 2] = 1.0F;

    ret[COL_NEIGHBOUR * 3 + 0] = 1.0F;
    ret[COL_NEIGHBOUR * 3 + 1] = 0.0F;
    ret[COL_NEIGHBOUR * 3 + 2] = 0.0F;

    *ncolours = NCOLOURS;
    return ret;
}

static game_drawstate *game_new_drawstate(game_state *state)
{
    struct game_drawstate *ds = snew(struct game_drawstate);

    ds->tilesize = 0;

    return ds;
}

static void game_free_drawstate(game_drawstate *ds)
{
    sfree(ds);
}

static point mix(point a, point b, float distance)
{
    point ret;

    ret.d = a.d * b.d;
    ret.x = a.x * b.d + distance * (b.x * a.d - a.x * b.d);
    ret.y = a.y * b.d + distance * (b.y * a.d - a.y * b.d);

    return ret;
}

static void game_redraw(frontend *fe, game_drawstate *ds, game_state *oldstate,
			game_state *state, int dir, game_ui *ui,
			float animtime, float flashtime)
{
    int w, h;
    edge *e;
    int i, j;
    int bg;

    /*
     * There's no terribly sensible way to do partial redraws of
     * this game, so I'm going to have to resort to redrawing the
     * whole thing every time.
     */

    bg = (flashtime != 0 ? COL_DRAGPOINT : COL_BACKGROUND);
    game_compute_size(&state->params, ds->tilesize, &w, &h);
    draw_rect(fe, 0, 0, w, h, bg);

    /*
     * Draw the edges.
     */

    for (i = 0; (e = index234(state->graph->edges, i)) != NULL; i++) {
	point p1, p2;
	int x1, y1, x2, y2;

	p1 = state->pts[e->a];
	p2 = state->pts[e->b];
	if (ui->dragpoint == e->a)
	    p1 = ui->newpoint;
	else if (ui->dragpoint == e->b)
	    p2 = ui->newpoint;

	if (oldstate) {
	    p1 = mix(oldstate->pts[e->a], p1, animtime / ui->anim_length);
	    p2 = mix(oldstate->pts[e->b], p2, animtime / ui->anim_length);
	}

	x1 = p1.x * ds->tilesize / p1.d;
	y1 = p1.y * ds->tilesize / p1.d;
	x2 = p2.x * ds->tilesize / p2.d;
	y2 = p2.y * ds->tilesize / p2.d;

	draw_line(fe, x1, y1, x2, y2, COL_LINE);
    }

    /*
     * Draw the points.
     * 
     * When dragging, we should not only vary the colours, but
     * leave the point being dragged until last.
     */
    for (j = 0; j < 3; j++) {
	int thisc = (j == 0 ? COL_POINT :
		     j == 1 ? COL_NEIGHBOUR : COL_DRAGPOINT);
	for (i = 0; i < state->params.n; i++) {
	    int x, y, c;
	    point p = state->pts[i];

	    if (ui->dragpoint == i) {
		p = ui->newpoint;
		c = COL_DRAGPOINT;
	    } else if (ui->dragpoint >= 0 &&
		       isedge(state->graph->edges, ui->dragpoint, i)) {
		c = COL_NEIGHBOUR;
	    } else {
		c = COL_POINT;
	    }

	    if (oldstate)
		p = mix(oldstate->pts[i], p, animtime / ui->anim_length);

	    if (c == thisc) {
		x = p.x * ds->tilesize / p.d;
		y = p.y * ds->tilesize / p.d;

#ifdef VERTEX_NUMBERS
		draw_circle(fe, x, y, DRAG_THRESHOLD, bg, bg);
		{
		    char buf[80];
		    sprintf(buf, "%d", i);
		    draw_text(fe, x, y, FONT_VARIABLE, DRAG_THRESHOLD*3/2,
			      ALIGN_VCENTRE|ALIGN_HCENTRE, c, buf);
		}
#else
		draw_circle(fe, x, y, CIRCLE_RADIUS, c, COL_OUTLINE);
#endif
	    }
	}
    }

    draw_update(fe, 0, 0, w, h);
}

static float game_anim_length(game_state *oldstate, game_state *newstate,
			      int dir, game_ui *ui)
{
    if (ui->just_moved)
	return 0.0F;
    if ((dir < 0 ? oldstate : newstate)->just_solved)
	ui->anim_length = SOLVEANIM_TIME;
    else
	ui->anim_length = ANIM_TIME;
    return ui->anim_length;
}

static float game_flash_length(game_state *oldstate, game_state *newstate,
			       int dir, game_ui *ui)
{
    if (!oldstate->completed && newstate->completed &&
	!oldstate->cheated && !newstate->cheated)
        return FLASH_TIME;
    return 0.0F;
}

static int game_wants_statusbar(void)
{
    return FALSE;
}

static int game_timing_state(game_state *state, game_ui *ui)
{
    return TRUE;
}

#ifdef COMBINED
#define thegame untangle
#endif

const struct game thegame = {
    "Untangle", "games.untangle",
    default_params,
    game_fetch_preset,
    decode_params,
    encode_params,
    free_params,
    dup_params,
    TRUE, game_configure, custom_params,
    validate_params,
    new_game_desc,
    validate_desc,
    new_game,
    dup_game,
    free_game,
    TRUE, solve_game,
    FALSE, game_text_format,
    new_ui,
    free_ui,
    encode_ui,
    decode_ui,
    game_changed_state,
    interpret_move,
    execute_move,
    PREFERRED_TILESIZE, game_compute_size, game_set_size,
    game_colours,
    game_new_drawstate,
    game_free_drawstate,
    game_redraw,
    game_anim_length,
    game_flash_length,
    game_wants_statusbar,
    FALSE, game_timing_state,
    SOLVE_ANIMATES,		       /* mouse_priorities */
};
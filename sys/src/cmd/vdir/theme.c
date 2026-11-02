#include <u.h>
#include <libc.h>
#include <bio.h>
#include <draw.h>
#include "theme.h"

static Image*
color(ulong rgb)
{
	Image *i;

	i = allocimage(display, Rect(0, 0, 1, 1), screen->chan, 1,
		(rgb << 8) | 0xff);
	if(i == nil)
		sysfatal("allocimage: %r");
	return i;
}

Theme*
loadtheme(void)
{
	Theme *theme;
	Biobuf *bp;
	char *s, *fields[3], *end;
	ulong rgb;
	int i, j;
	struct { char *name; ulong rgb; Image **image; } colors[] = {
		{ "back", 0xffffff, nil },
		{ "text", 0x000000, nil },
		{ "border", 0x999999, nil },
		{ "high", 0xcccccc, nil },
		{ "htext", 0x000000, nil },
		{ "dir", 0x5555ff, nil },
		{ "exec", 0x55ff55, nil },
		{ "link", 0x55ffff, nil },
		{ "dev", 0xffff55, nil },
		{ "pipe", 0xffff55, nil },
		{ "broken", 0xff5555, nil },
		{ "socket", 0xff55ff, nil },
		{ "titleback", ~0UL, nil },
	};

	theme = mallocz(sizeof *theme, 1);
	if(theme == nil)
		sysfatal("out of memory");
	colors[0].image = &theme->back;
	colors[1].image = &theme->text;
	colors[2].image = &theme->border;
	colors[3].image = &theme->high;
	colors[4].image = &theme->htext;
	colors[5].image = &theme->dir;
	colors[6].image = &theme->exec;
	colors[7].image = &theme->link;
	colors[8].image = &theme->dev;
	colors[9].image = &theme->pipe;
	colors[10].image = &theme->broken;
	colors[11].image = &theme->socket;
	colors[12].image = &theme->title;
	/* Missing or partial themes retain usable defaults. */
	bp = Bopen("/dev/theme", OREAD);
	if(bp != nil){
		while((s = Brdstr(bp, '\n', 1)) != nil){
			if(tokenize(s, fields, nelem(fields)) == 2 && strlen(fields[1]) == 6){
				for(j = 0; j < 6; j++)
					if(strchr("0123456789abcdefABCDEF", fields[1][j]) == nil)
						break;
				rgb = strtoul(fields[1], &end, 16);
				if(j == 6 && *end == 0)
					for(i = 0; i < nelem(colors); i++)
						if(strcmp(fields[0], colors[i].name) == 0)
							colors[i].rgb = rgb;
			}
			free(s);
		}
		Bterm(bp);
	}
	for(i = 0; i < nelem(colors); i++)
		if(colors[i].rgb != ~0UL)
			*colors[i].image = color(colors[i].rgb);
	return theme;
}

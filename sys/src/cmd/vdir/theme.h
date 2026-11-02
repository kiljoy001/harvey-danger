typedef struct Theme Theme;

struct Theme
{
	Image *back;
	Image *high;
	Image *border;
	Image *text;
	Image *htext;
	Image *dir;
	Image *exec;
	Image *link;
	Image *dev;
	Image *pipe;
	Image *broken;
	Image *socket;
	Image *title;
	Image *ltitle;
	Image *hold;
	Image *lhold;
	Image *palehold;
	Image *paletext;
	Image *size;
	Image *menubar;
	Image *menuback;
	Image *menuhigh;
	Image *menubord;
	Image *menutext;
	Image *menuhtext;
};

Theme* loadtheme(void);

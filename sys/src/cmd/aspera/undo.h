typedef struct UndoState UndoState;
typedef struct UndoStack UndoStack;

struct UndoState {
	UndoState *next;
	UndoState *prev;
	Rune *data;
	int datalen;
	int cursorpos;
	int type;
	int boundary;
};

struct UndoStack {
	UndoState *head;
	UndoState *current;
	int maxstates;
	int count;
	int pause;
	int dirty;
	int boundary;
};

UndoStack* undostackcreate(int maxstates);
void undostackfree(UndoStack *stack);
int undoaddstate(UndoStack *stack, int type, const Rune *data, int datalen, int cursorpos);
int undoaddinsert(UndoStack *stack, const Rune *data, int datalen, int cursorpos);
int undoadddelete(UndoStack *stack, const Rune *data, int datalen, int cursorpos);
int undoundo(UndoStack *stack, Rune **data, int *datalen, int *cursorpos, int *type);
int undoredo(UndoStack *stack, Rune **data, int *datalen, int *cursorpos, int *type);
int undocanundo(UndoStack *stack);
int undocanredo(UndoStack *stack);
void undoclear(UndoStack *stack);
void undopause(UndoStack *stack);
void undoresume(UndoStack *stack);
void undoboundary(UndoStack *stack);
int undoisboundary(UndoStack *stack);
void undolist(UndoStack *stack, Completion *c);
int undoaddfile(UndoStack *stack, const Rune *data, int datalen);
void undofileload(UndoStack *stack, char *name);
void undofilesave(UndoStack *stack, char *name);

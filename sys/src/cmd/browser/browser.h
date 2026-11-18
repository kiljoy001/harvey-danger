#ifndef BROWSER_H
#define BROWSER_H

void	cssfsserve(int fd);
void	cssfsserver(char *name);
void	htmlfsserve(int fd);
void	htmlfsserver(char *name);
void	jsfsserve(int fd);
void	jsfsserver(char *name);
int	jsrun(char *code, char **out);
void	layoutfsserve(int fd);
void	layoutfsserver(char *name);
void	paintfsserve(int fd);
void	paintfsserver(char *name);
void	textfsserve(int fd);
void	textfsserver(char *name);

#endif

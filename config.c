# include	<stdio.h>
# include	<stdlib.h>
# include	<string.h>
# include	<unistd.h>
# include	<expat.h>
# include	"picpgm.h"

#ifdef XML_LARGE_SIZE
#  define XML_FMT_INT_MOD "ll"
#else
#  define XML_FMT_INT_MOD "l"
#endif
#ifdef XML_UNICODE_WCHAR_T
#  define XML_FMT_STR "ls"
#else
#  define XML_FMT_STR "s"
#endif

static void XMLCALL start(void *data, const XML_Char *el, const XML_Char **attr);
static void XMLCALL end(void *data, const XML_Char *el);
static const char *findval(const XML_Char **attr, char *name);

static int state;
static char hier[BUFSIZ];
static size_t hierlen = 0;

extern int debug;

/*
 * NAME: load_config
 * PURPOSE: To load a picpgm config file
 * ARGUMENTS: pathname: path name of config file
 *	user_data: address of data to be passed to handlers
 * RETURNS: 0 on failure, non-0 on success
 */
int
load_config(const char *pathname, void *user_data)
{
    XML_Parser parser;
    FILE *fp;
    void *buf;

    if ((fp = fopen(pathname, "rt")) == NULL)
        return 0;

    parser = XML_ParserCreate(NULL);
    XML_SetElementHandler(parser, start, end);
    XML_SetUserData(parser, user_data);

    buf = XML_GetBuffer(parser, BUFSIZ);

    state = 0;
    while (1)
    {
        ssize_t len;
	int done;

	len = fread(buf, 1, BUFSIZ, fp);
	done = (len == 0);


	if (XML_ParseBuffer(parser, len, done) == XML_STATUS_ERROR) {
	    fprintf(stderr, "Parse error at line %" XML_FMT_INT_MOD "u:\n%" XML_FMT_STR "\n",
			    XML_GetCurrentLineNumber(parser),
			    XML_ErrorString(XML_GetErrorCode(parser)));
	    fclose(fp);
	    return 0;
	}

	if (done)
	    break;
    }

    XML_ParserFree(parser);
    fclose(fp);

    return 1;
}

/*
 * NAME: start
 * PURPOSE: To handle an XML start tag
 * ARGUMENTS: data: user-supplied data
 *	el: tag name
 *	attr: attribute name/value pairs
 * RETURNS: Nothing
 * NOTE: This is a callback from XML_Parse()
 */
static void XMLCALL
start(void *data, const XML_Char *el, const XML_Char **attr)
{
    int i;
    char *sep = ".";
    struct pin *pins = data;

    if (hierlen == 0)
        sep = "";
    hierlen += sprintf(&hier[hierlen], "%s%s", sep, el);

    if (debug)
    {
	fprintf(stderr, "%d: start(\"%s\") -> %s\n", state, el, hier);
	for (i = 0; attr[i]; i += 2)
	    fprintf(stderr, " %" XML_FMT_STR "='%" XML_FMT_STR "'", attr[i], attr[i + 1]);
	fputc('\n', stderr);
    }

    if (strcmp(hier, "Config.PgmIf") == 0 && strcmp(findval(attr, "name"), "GPIO Programmer (Raspberry Pi)") == 0)
        state = 1;
    if (state == 1 && strcmp(hier, "Config.PgmIf.PinCfg") == 0)
    {
        const char *name, *pin, *invert;

	if (debug)
	    fprintf(stderr, "*** \"Config.PgmIf.PinCfg\" ***\n");

	name = findval(attr, "name");
	pin = findval(attr, "pin");
	invert = findval(attr, "invert");

	for (; pins->name != NULL && strcmp(pins->name, name) != 0; pins++)
	    ;
	if (pins->name == NULL)
	    return;

	if (debug)
	    fprintf(stderr, "Setting %s pin=%s invert=%s\n", pins->name, pin, invert);
	pins->pin = atoi(pin);
	pins->invert = atoi(invert);
    }
}

/*
 * NAME: end
 * PURPOSE: To handle an end tag
 * ARGUMENTS: data: user-supplied data
 *	el: tag name
 * RETURNS: Nothing.
 * NOTE: This is a callback from XML_Parse()
 */
static void XMLCALL
end(void *data, const XML_Char *el)
{
    char *period;

    if (state == 1 && strcmp(hier, "Config.PgmIf") == 0)
        state = 0;

    if ((period = strrchr(hier, '.')) == NULL)
        period = hier;

    *period = '\0';
    hierlen = period - hier;

    if (debug)
        fprintf(stderr, "end(\"%s\") -> %s\n", el, hier);
}

/*
 * NAME: findval
 * PURPOSE: To find a value in an array of name/value pairs
 *	given a name
 * ARGUMENTS: attr: attribute name/value pairs
 *	name: name to search for
 * RETURNS: pointer to value string or NULL if name not found
 */
static const char *
findval(const XML_Char **attr, char *name)
{
    int i;

    for (i = 0; attr[i] != NULL && strcmp(attr[i], name) != 0; i += 2)
	;

    return (attr[i] == NULL) ? "" : attr[i+1];
}
/* ********************** END XML code ********************** */

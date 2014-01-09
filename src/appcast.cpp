/*
 *  This file is part of WinSparkle (http://winsparkle.org)
 *
 *  Copyright (C) 2009-2013 Vaclav Slavik
 *
 *  Permission is hereby granted, free of charge, to any person obtaining a
 *  copy of this software and associated documentation files (the "Software"),
 *  to deal in the Software without restriction, including without limitation
 *  the rights to use, copy, modify, merge, publish, distribute, sublicense,
 *  and/or sell copies of the Software, and to permit persons to whom the
 *  Software is furnished to do so, subject to the following conditions:
 *
 *  The above copyright notice and this permission notice shall be included in
 *  all copies or substantial portions of the Software.
 *
 *  THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 *  IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 *  FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 *  AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 *  LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
 *  FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER
 *  DEALINGS IN THE SOFTWARE.
 *
 */

#include "appcast.h"
#include "error.h"

#include <expat.h>
#include <vector>
#include <algorithm>

using namespace std;

namespace winsparkle
{

/*--------------------------------------------------------------------------*
version comparison
*--------------------------------------------------------------------------*/

// Note: This code is based on Sparkle's SUStandardVersionComparator by
//       Andy Matuschak.

namespace
{

	// String characters classification. Valid components of version numbers
	// are numbers, period or string fragments ("beta" etc.).
	enum CharType
	{
		Type_Number,
		Type_Period,
		Type_String
	};

	CharType ClassifyChar(char c)
	{
		if (c == '.')
			return Type_Period;
		else if (c >= '0' && c <= '9')
			return Type_Number;
		else
			return Type_String;
	}

	// Split version string into individual components. A component is continuous
	// run of characters with the same classification. For example, "1.20rc3" would
	// be split into ["1",".","20","rc","3"].
	vector<string> SplitVersionString(const string& version)
	{
		vector<string> list;

		if (version.empty())
			return list; // nothing to do here

		string s;
		const size_t len = version.length();

		s = version[0];
		CharType prevType = ClassifyChar(version[0]);

		for (size_t i = 1; i < len; i++)
		{
			const char c = version[i];
			const CharType newType = ClassifyChar(c);

			if (prevType != newType || prevType == Type_Period)
			{
				// We reached a new segment. Period gets special treatment,
				// because "." always delimiters components in version strings
				// (and so ".." means there's empty component value).
				list.push_back(s);
				s = c;
			}
			else
			{
				// Add character to current segment and continue.
				s += c;
			}

			prevType = newType;
		}

		// Don't forget to add the last part:
		list.push_back(s);

		return list;
	}

} // anonymous namespace


int CompareVersions(const string& verA, const string& verB)
{
	const vector<string> partsA = SplitVersionString(verA);
	const vector<string> partsB = SplitVersionString(verB);

	// Compare common length of both version strings.
	const size_t n = min(partsA.size(), partsB.size());
	for (size_t i = 0; i < n; i++)
	{
		const string& a = partsA[i];
		const string& b = partsB[i];

		const CharType typeA = ClassifyChar(a[0]);
		const CharType typeB = ClassifyChar(b[0]);

		if (typeA == typeB)
		{
			if (typeA == Type_String)
			{
				int result = a.compare(b);
				if (result != 0)
					return result;
			}
			else if (typeA == Type_Number)
			{
				const int intA = atoi(a.c_str());
				const int intB = atoi(b.c_str());
				if (intA > intB)
					return 1;
				else if (intA < intB)
					return -1;
			}
		}
		else // components of different types
		{
			if (typeA != Type_String && typeB == Type_String)
			{
				// 1.2.0 > 1.2rc1
				return 1;
			}
			else if (typeA == Type_String && typeB != Type_String)
			{
				// 1.2rc1 < 1.2.0
				return -1;
			}
			else
			{
				// One is a number and the other is a period. The period
				// is invalid.
				return (typeA == Type_Number) ? 1 : -1;
			}
		}
	}

	// The versions are equal up to the point where they both still have
	// parts. Lets check to see if one is larger than the other.
	if (partsA.size() == partsB.size())
		return 0; // the two strings are identical

	// Lets get the next part of the larger version string
	// Note that 'n' already holds the index of the part we want.

	int shorterResult, longerResult;
	CharType missingPartType; // ('missing' as in "missing in shorter version")

	if (partsA.size() > partsB.size())
	{
		missingPartType = ClassifyChar(partsA[n][0]);
		shorterResult = -1;
		longerResult = 1;
	}
	else
	{
		missingPartType = ClassifyChar(partsB[n][0]);
		shorterResult = 1;
		longerResult = -1;
	}

	if (missingPartType == Type_String)
	{
		// 1.5 > 1.5b3
		return shorterResult;
	}
	else
	{
		// 1.5.1 > 1.5
		return longerResult;
	}
}

/*--------------------------------------------------------------------------*
                                XML parsing
 *--------------------------------------------------------------------------*/

namespace
{

#define MVAL(x) x
#define CONCAT3(a,b,c) MVAL(a)##MVAL(b)##MVAL(c)

#define NS_SPARKLE      "http://www.andymatuschak.org/xml-namespaces/sparkle"
#define NS_SEP          '#'
#define NS_SPARKLE_NAME(name) NS_SPARKLE "#" name

#define NODE_CHANNEL    "channel"
#define NODE_ITEM       "item"
#define NODE_RELNOTES   NS_SPARKLE_NAME("releaseNotesLink")
#define NODE_TITLE "title"
#define NODE_DESCRIPTION "description"
#define NODE_ENCLOSURE  "enclosure"
#define ATTR_URL        "url"
#define ATTR_VERSION    NS_SPARKLE_NAME("version")
#define ATTR_SHORTVERSION NS_SPARKLE_NAME("shortVersionString")


// context data for the parser
struct ContextData
{
    ContextData(Appcast& a, XML_Parser& p)
        : appcast(a),
          parser(p),
          in_channel(0), in_item(0), in_relnotes(0), in_title(0), in_description(0)
    {}

    // appcast we're parsing into
    Appcast& appcast;

    // the parser we're using
    XML_Parser& parser;

    // is inside <channel>, <item> or <sparkle:releaseNotesLink>, <title>, or <description> respectively?
    int in_channel, in_item, in_relnotes, in_title, in_description;
};

static std::string last_version;

void XMLCALL OnStartElement(void *data, const char *name, const char **attrs)
{
    ContextData& ctxt = *static_cast<ContextData*>(data);

    if ( strcmp(name, NODE_CHANNEL) == 0 )
    {
        ctxt.in_channel++;
    }
    else if ( ctxt.in_channel && strcmp(name, NODE_ITEM) == 0 )
    {
        ctxt.in_item++;
    }
    else if ( ctxt.in_item )
    {
        if ( strcmp(name, NODE_RELNOTES) == 0 )
        {
            ctxt.in_relnotes++;
        }
        else if ( strcmp(name, NODE_TITLE) == 0 )
        {
            ctxt.in_title++;
        }
        else if ( strcmp(name, NODE_DESCRIPTION) == 0 )
        {
            ctxt.in_description++;
        }
        else if (strcmp(name, NODE_ENCLOSURE) == 0 )
        {
			int go = 1;
			if (last_version.length() > 0)
			{
				go = 0;

				for (int i = 0; attrs[i]; i += 2)
				{
					if (strcmp(attrs[i], ATTR_VERSION) == 0)
					{
						if (CompareVersions(last_version, attrs[i + 1]) < 0)
						{
							go = 1;
						}
					}
				}
			}

			if (go == 1)
			{
				for (int i = 0; attrs[i]; i += 2)
				{
					const char *name = attrs[i];
					const char *value = attrs[i + 1];

					if (strcmp(name, ATTR_URL) == 0)
						ctxt.appcast.DownloadURL = value;
					else if (strcmp(name, ATTR_VERSION) == 0)
					{
						ctxt.appcast.Version = value;
						last_version = attrs[i + 1];
					}
					else if (strcmp(name, ATTR_SHORTVERSION) == 0)
						ctxt.appcast.ShortVersionString = value;
				}
			}
        }
    }
}


void XMLCALL OnEndElement(void *data, const char *name)
{
    ContextData& ctxt = *static_cast<ContextData*>(data);

    if ( ctxt.in_item && strcmp(name, NODE_RELNOTES) == 0 )
    {
        ctxt.in_relnotes--;
    }
    else if ( ctxt.in_item && strcmp(name, NODE_TITLE) == 0 )
    {
        ctxt.in_title--;
    }
    else if ( ctxt.in_item && strcmp(name, NODE_DESCRIPTION) == 0 )
    {
        ctxt.in_description--;
    }
    else if ( ctxt.in_channel && strcmp(name, NODE_ITEM) == 0 )
    {
        // One <item> in the channel is enough to get the information we
        // need, stop parsing now that we processed it.
        //XML_StopParser(ctxt.parser, XML_TRUE);
		// actually, read all of them
    }
    else if ( strcmp(name, NODE_CHANNEL) == 0 )
    {
        ctxt.in_channel--;
    }
}


void XMLCALL OnText(void *data, const char *s, int len)
{
    ContextData& ctxt = *static_cast<ContextData*>(data);

    if ( ctxt.in_relnotes )
        ctxt.appcast.ReleaseNotesURL.append(s, len);
    else if ( ctxt.in_title )
        ctxt.appcast.Title.append(s, len);
    else if ( ctxt.in_description )
        ctxt.appcast.Description.append(s, len);
}

} // anonymous namespace


/*--------------------------------------------------------------------------*
                               Appcast class
 *--------------------------------------------------------------------------*/

void Appcast::Load(const std::string& xml)
{
    XML_Parser p = XML_ParserCreateNS(NULL, NS_SEP);
    if ( !p )
        throw std::runtime_error("Failed to create XML parser.");

    ContextData ctxt(*this, p);

    XML_SetUserData(p, &ctxt);
    XML_SetElementHandler(p, OnStartElement, OnEndElement);
    XML_SetCharacterDataHandler(p, OnText);

    XML_Status st = XML_Parse(p, xml.c_str(), xml.size(), XML_TRUE);

    if ( st == XML_STATUS_ERROR )
    {
        std::string msg("XML parser error: ");
        msg.append(XML_ErrorString(XML_GetErrorCode(p)));
        XML_ParserFree(p);
        throw std::runtime_error(msg);
    }

    XML_ParserFree(p);
}

} // namespace winsparkle

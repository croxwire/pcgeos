/***********************************************************************
 *
 *	Copyright FreeGEOS-Project
 *
 * PROJECT:	  FreeGEOS
 * MODULE:	  TrueType font driver
 * FILE:	  ttinit.c
 *
 * AUTHOR:	  Jirka Kunze: December 20 2022
 *
 * REVISION HISTORY:
 *	Date	  Name	    Description
 *	----	  ----	    -----------
 *	20/12/22  JK	    Initial version
 *
 * DESCRIPTION:
 *	Definition of driver function DR_INIT.
 ***********************************************************************/

#include "ttinit.h"
#include "ttadapter.h"
#include "ttcharmapper.h"
#include <fileEnum.h>
#include <geos.h>
#include <heap.h>
#include <lmem.h>
#include <ec.h>
#include <initfile.h>
#include <unicode.h>


static TT_Error ProcessFont( const char* file, MemHandle fontInfoBlock );

static sword getFontIDAvailIndex( 
                        FontID          fontID, 
                        MemHandle       fontInfoBlock );

static FontID getFontID( const char* familiyName );

static FontAttrs mapFamilyClass( TT_Short familyClass );

static FontWeight mapFontWeight( TT_Short weightClass );

static TextStyle mapTextStyle( const char* subfamily );

static word getNameFromNameTable( 
                        char*           name, 
                        TT_Face         face, 
                        TT_UShort       nameIndex );

static void convertHeader( 
                        TT_Face             face, 
                        TT_Face_Properties  faceProperties, 
                        TT_CharMap          charMap, 
                        FontHeader*         fontHeader );

static word toHash( const char* str );

static int  strlen( const char* str );

static void strcpy( char* dest, const char* source );


/********************************************************************
 *                      Init_FreeType
 ********************************************************************
 * SYNOPSIS:	  Initialises the FreeType Engine with the kerning 
 *                extension. This is the adapter function for DR_INIT.
 * 
 * PARAMETERS:    void
 * 
 * RETURNS:       TT_Error = FreeType errorcode (see tterrid.h)
 * 
 * SIDE EFFECTS:  none
 * 
 * STRATEGY:      Initialises the FreeType engine by delegating to 
 *                TT_Init_FreeType() and TT_Init_Kerning().
 * 
 * REVISION HISTORY:
 *      Date      Name      Description
 *      ----      ----      -----------
 *      7/15/22   JK        Initial Revision
 *******************************************************************/

TT_Error _pascal Init_FreeType()
{
        TT_Error        error;
 
 
        error = TT_Init_FreeType();
        if ( error != TT_Err_Ok )
                return error;

        //commented out because it freezes swat
        //TT_Init_Kerning()

        return TT_Err_Ok;
}


/********************************************************************
 *                      Exit_FreeType
 ********************************************************************
 * SYNOPSIS:	  Deinitialises the FreeType Engine. This is the 
 *                adapter function for DR_EXIT.
 * 
 * PARAMETERS:    void
 * 
 * RETURNS:       TT_Error = FreeType errorcode (see tterrid.h)
 * 
 * SIDE EFFECTS:  none
 * 
 * STRATEGY:      Deinitialises the FreeType engine by delegating to 
 *                TT_Done_FreeType().
 * 
 * REVISION HISTORY:
 *      Date      Name      Description
 *      ----      ----      -----------
 *      7/15/22   JK        Initial Revision
 *******************************************************************/

TT_Error _pascal Exit_FreeType() 
{
        return TT_Done_FreeType();
}


/********************************************************************
 *                      TrueType_InitFonts
 ********************************************************************
 * SYNOPSIS:	  Search for TTF fonts and register them. This is the 
 *                adapter function for DR_FONT_INIT_FONTS.
 * 
 * PARAMETERS:    fontInfoBlock         MemHandle to fontInfo.
 * 
 * RETURNS:       void
 * 
 * SIDE EFFECTS:  none
 * 
 * STRATEGY:      
 * 
 * REVISION HISTORY:
 *      Date      Name      Description
 *      ----      ----      -----------
 *      7/15/22   JK        Initial Revision
 *******************************************************************/
void _pascal TrueType_InitFonts( MemHandle fontInfoBlock )
{
        FileEnumParams   ttfEnumParams;
        word             numOtherFiles;
        word             numFiles;
        word             file;
        FileLongName*    ptrFileName;
        MemHandle        fileEnumBlock    = NullHandle;
        FileExtAttrDesc  ttfExtAttrDesc[] =
                { { FEA_NAME, 0, sizeof( FileLongName ), NULL },
                  { FEA_END_OF_LIST, 0, 0, NULL } };


        FilePushDir();

        /* go to font/ttf directory */
        FileSetCurrentPath( SP_FONT, TTF_DIRECTORY );

        /* get all filenames contained in current directory */
        ttfEnumParams.FEP_searchFlags   = FESF_NON_GEOS;
        ttfEnumParams.FEP_returnAttrs   = ttfExtAttrDesc;
        ttfEnumParams.FEP_returnSize    = sizeof( FileLongName );
        ttfEnumParams.FEP_matchAttrs    = NullHandle;
        ttfEnumParams.FEP_bufSize       = FE_BUFSIZE_UNLIMITED;
        ttfEnumParams.FEP_skipCount     = 0;
        ttfEnumParams.FEP_callback      = NullHandle;
        ttfEnumParams.FEP_callbackAttrs = NullHandle;
        ttfEnumParams.FEP_headerSize    = 0;

        numFiles = FileEnum( &ttfEnumParams, &fileEnumBlock, &numOtherFiles );
        ECCheckMemHandle( fileEnumBlock );

        if( numFiles == 0 )
                goto Fin;

        /* iterate over all filenames and try to register a font.*/
        ptrFileName = MemLock( fileEnumBlock );
        for( file = 0; file < numFiles; file++ )
                ProcessFont( ptrFileName++, fontInfoBlock );

        MemFree( fileEnumBlock );

Fin:
        FilePopDir();
}


/********************************************************************
 *                      ProcessFont
 ********************************************************************
 * SYNOPSIS:	  Registers a font with its styles as an available font.
 * 
 * PARAMETERS:    fileName              Name of font file.
 *                fontInfoBlock         Handle to memory block with all
 *                                      infos about aviable fonts.
 * 
 * RETURNS:       TT_Error              Error code (see fterror.h).
 * 
 * SIDE EFFECTS:  none
 * 
 * STRATEGY:      
 * 
 * REVISION HISTORY:
 *      Date      Name      Description
 *      ----      ----      -----------
 *      20/01/23  JK        Initial Revision
 *******************************************************************/

static TT_Error ProcessFont( const char* fileName, MemHandle fontInfoBlock )
{
        FileHandle          truetypeFile;
        TT_Face             face;
        TT_CharMap          charMap;
        TT_Face_Properties  faceProperties;
        TT_Error            error = TT_Err_Ok;
        char                familyName[FID_NAME_LEN];
        char                styleName[STYLE_NAME_LENGTH];
        FontID              fontID;
        sword               availIndex;


        ECCheckBounds( (void*)fileName );
        ECCheckMemHandle( fontInfoBlock );


        truetypeFile = FileOpen( fileName, FILE_ACCESS_R | FILE_DENY_W );
        
        ECCheckFileHandle( truetypeFile );

        error = TT_Open_Face( truetypeFile, &face );
        if( error )
                goto Fin;

        error = TT_Get_Face_Properties( face, &faceProperties );
        if( error )
                goto Fail;

        error = getCharMap( face, &charMap );
        if ( error != TT_Err_Ok )
                goto Fail;

        if ( getNameFromNameTable( familyName, face, FAMILY_NAME_ID ) == 0 )
                goto Fail;

        if ( getNameFromNameTable( styleName, face, STYLE_NAME_ID ) == 0 )
                goto Fail;

        fontID = getFontID( familyName );
	availIndex = getFontIDAvailIndex( fontID, fontInfoBlock );

        /* if we have an new font FontAvailEntry, FontInfo and Outline must be created */
        if ( availIndex < 0 )
        {
		ChunkHandle            fontInfoChunk;
		FontsAvailEntry*       newEntry;
		FontInfo*              fontInfo;
                TrueTypeOutlineEntry*  trueTypeOutlineEntry;
                ChunkHandle            trueTypeOutlineChunk;
                OutlineDataEntry*      outlineDataEntry;
                ChunkHandle            fontHeaderChunk;
                FontHeader*            fontHeader;
		
		/* allocate chunk for FontsAvailEntry and fill it */
		if( LMemInsertAtHandles( fontInfoBlock, sizeof(LMemBlockHeader), 0, sizeof(FontsAvailEntry) ) ) 
		{
			error = TT_Err_Out_Of_Memory;
			goto Fail;
		}
		newEntry = LMemDeref( ConstructOptr( fontInfoBlock, sizeof( LMemBlockHeader ) ) );
                newEntry->FAE_fontID = fontID;
                newEntry->FAE_infoHandle = NullChunk;
                *newEntry->FAE_fileName = 0;

		/* allocate chunk for FontInfo and OutlineDataEntry */
		fontInfoChunk = LMemAlloc( fontInfoBlock, sizeof(OutlineDataEntry) + sizeof(FontInfo) );
		if( fontInfoChunk == NullChunk )
		{
			/* revert previous allocation of FontsAvailEntry */
			LMemDeleteAtHandles( fontInfoBlock, sizeof(LMemBlockHeader), 0, sizeof(FontsAvailEntry) );
			error = TT_Err_Out_Of_Memory;
			goto Fail;
		}

                /* get pointer to FontInfo and fill it */
		fontInfo = LMemDerefHandles( fontInfoBlock, fontInfoChunk );
                strcpy( fontInfo->FI_faceName, familyName );
                fontInfo->FI_fileHandle   = NullHandle;
                fontInfo->FI_fontID       = fontID;
                fontInfo->FI_family       = mapFamilyClass( faceProperties.os2->sFamilyClass );
                fontInfo->FI_maker        = FM_TRUETYPE;
                fontInfo->FI_pointSizeTab = 0;
                fontInfo->FI_pointSizeEnd = 0;
                fontInfo->FI_outlineTab   = 0;
                fontInfo->FI_outlineEnd   = 0;

		/* add Chunk for TrueTypeOutlineEntry */
		trueTypeOutlineChunk = LMemAlloc( fontInfoBlock, sizeof(TrueTypeOutlineEntry) );
		if( trueTypeOutlineChunk == NullChunk)
		{
			LMemFreeHandles( fontInfoBlock, fontInfoChunk );
			LMemDeleteAtHandles( fontInfoBlock, sizeof(LMemBlockHeader), 0, sizeof(FontsAvailEntry) );
			goto Fail;
		}

                /* add chunk for FontHeader */
                fontHeaderChunk = LMemAlloc( fontInfoBlock, sizeof(FontHeader) );
                if( fontHeaderChunk == NullChunk )
                {
                        LMemFreeHandles( fontInfoBlock, trueTypeOutlineChunk );
                        LMemFreeHandles( fontInfoBlock, fontInfoChunk );
                        LMemDeleteAtHandles( fontInfoBlock, sizeof(LMemBlockHeader), 0, sizeof(FontsAvailEntry) );
                        goto Fail;
                }

		fontInfo = LMemDerefHandles( fontInfoBlock, fontInfoChunk );
		trueTypeOutlineEntry = LMemDerefHandles( fontInfoBlock, trueTypeOutlineChunk );

                /* fill TrueTypeOutlineEntry */
                strcpy( trueTypeOutlineEntry->TTOE_fontFileName, fileName );
                
                /* fill OutlineDataEntry */
                outlineDataEntry = (OutlineDataEntry*) (fontInfo + 1);
                outlineDataEntry->ODE_style  = mapTextStyle( styleName );
                outlineDataEntry->ODE_weight = mapFontWeight( faceProperties.os2->usWeightClass );
                outlineDataEntry->ODE_header.OE_handle = trueTypeOutlineChunk;
                outlineDataEntry->ODE_first.OE_handle = fontHeaderChunk;
	
                /* fill FontHeader */
                fontHeader = LMemDerefHandles( fontInfoBlock, fontHeaderChunk );
                convertHeader( face, faceProperties, charMap, fontHeader );

		fontInfo->FI_outlineTab = sizeof( FontInfo );
		fontInfo->FI_outlineEnd = sizeof( FontInfo ) + sizeof( OutlineDataEntry );

                /* set Chunk to FontInfo into FontsAvailEntry */
                newEntry = LMemDeref( ConstructOptr( fontInfoBlock, sizeof( LMemBlockHeader ) ) );
                newEntry->FAE_infoHandle = fontInfoChunk;
	}
        else
        {
		ChunkHandle           trueTypeOutlineChunk;
                TrueTypeOutlineEntry* trueTypeOutlineEntry;
                ChunkHandle           fontHeaderChunk;
                FontHeader*           fontHeader;
		FontInfo*             fontInfo;
                FontsAvailEntry*      availEntries = LMemDeref( ConstructOptr(fontInfoBlock, sizeof(LMemBlockHeader)) );
                ChunkHandle           fontInfoChunk = availEntries[availIndex].FAE_infoHandle;
		OutlineDataEntry*     outlineData = (OutlineDataEntry*) (((byte*)fontInfo) + fontInfo->FI_outlineTab);
                OutlineDataEntry*     outlineDataEnd = (OutlineDataEntry*) (((byte*)fontInfo) + fontInfo->FI_outlineEnd);

		while( outlineData < outlineDataEnd)
		{
                        if( ( mapTextStyle( styleName ) == outlineData->ODE_style ) &&
	                    ( mapFontWeight( faceProperties.os2->usWeightClass ) == outlineData->ODE_weight ) )
			{
				goto Fail;
			}
			outlineData++;
		}

		/* not found append new outline entry */
		trueTypeOutlineChunk = LMemAlloc( fontInfoBlock, sizeof(TrueTypeOutlineEntry) );
		if( trueTypeOutlineChunk == NullChunk )
		{
			error = TT_Err_Out_Of_Memory;
			goto Fail;			
		}

                /* insert OutlineDataEntry behinde fontinfo */
                fontInfo = LMemDeref( ConstructOptr(fontInfoBlock, fontInfoChunk) );
                if( LMemInsertAtHandles( fontInfoBlock, fontInfoChunk, fontInfo->FI_outlineTab, sizeof( OutlineDataEntry ) ) )
		{
			LMemFreeHandles( fontInfoBlock, trueTypeOutlineChunk );
			error = TT_Err_Out_Of_Memory;
			goto Fail;
		}

                /* add chunk for FontHeader */
                fontHeaderChunk = LMemAlloc( fontInfoBlock, sizeof(FontHeader) );
                if( fontHeaderChunk == NullChunk )
                {
                        LMemFreeHandles( fontInfoBlock, trueTypeOutlineChunk );
                        error = TT_Err_Out_Of_Memory;
                        goto Fail;
                }
	
                /* fill TrueTypeOutlineEntry */
                trueTypeOutlineEntry = LMemDerefHandles( fontInfoBlock, trueTypeOutlineChunk );
                strcpy( trueTypeOutlineEntry->TTOE_fontFileName, fileName );
                
                /* fill OutlineDataEntry */
                fontInfo = LMemDeref( ConstructOptr(fontInfoBlock, fontInfoChunk) );
                outlineData = (OutlineDataEntry*) (fontInfo + 1);
                outlineData->ODE_style  = mapTextStyle( styleName );
                outlineData->ODE_weight = mapFontWeight( faceProperties.os2->usWeightClass );
                outlineData->ODE_header.OE_handle = trueTypeOutlineChunk;
                outlineData->ODE_first.OE_handle = fontHeaderChunk;

                /* fill FontHeader */
                fontHeader = LMemDerefHandles( fontInfoBlock, fontHeaderChunk );
                convertHeader( face, faceProperties, charMap, fontHeader );
		
		fontInfo = LMemDeref( ConstructOptr(fontInfoBlock, fontInfoChunk) );
        	fontInfo->FI_outlineEnd += sizeof( OutlineDataEntry );
	}

Fail:
        TT_Close_Face( face );
Fin:        
        FileClose( truetypeFile, FALSE );
        return error;
}


/********************************************************************
 *                      toHash
 ********************************************************************
 * SYNOPSIS:	  Calculates the hash value of the passed string.
 * 
 * PARAMETERS:    str           Pointer to the string.
 * 
 * RETURNS:       word          Hash value for passed string.
 * 
 * SIDE EFFECTS:  none
 * 
 * STRATEGY:      
 * 
 * REVISION HISTORY:
 *      Date      Name      Description
 *      ----      ----      -----------
 *      21/01/23  JK        Initial Revision
 *******************************************************************/

static word toHash( const char* str )
{
        word    i;
        dword   hash = strlen( str );

        for ( i = 0; i < strlen( str ) ; ++i )
		hash = ( ( hash * 7 ) % 65535 ) + str[i];

        return (word) hash;
}


/********************************************************************
 *                      mapFamiliClass
 ********************************************************************
 * SYNOPSIS:	  Maps the TrueType family class to FreeGEOS FontAttrs.
 * 
 * PARAMETERS:    familyClass           TrueType family class.
 * 
 * RETURNS:       FontAttrs             FreeGEOS FontAttrs.
 * 
 * SIDE EFFECTS:  none
 * 
 * STRATEGY:      
 * 
 * REVISION HISTORY:
 *      Date      Name      Description
 *      ----      ----      -----------
 *      21/01/23  JK        Initial Revision
 *******************************************************************/

static FontAttrs mapFamilyClass( TT_Short familyClass ) 
{
        byte        class    = familyClass >> 8;
        byte        subclass = (byte) familyClass & 0x00ff;
        FontFamily  family;

        switch ( class )
        {
        case 1:         //old style serifs
        case 2:         //transitional serifs
        case 3:         //modern serifs
                family = FF_SERIF;
                break;
        case 4:         //clarendon serifs
                family = subclass == 6 ? FF_MONO : FF_SERIF;
                break;
        case 5:         //slab serifs
                family = subclass == 1 ? FF_MONO : FF_SERIF;
                break;
                        //6 = reserved
        case 7:         //freeform serfis
                family = FF_SERIF;
                break;
        case 8:         //sans serif
                family = FF_SANS_SERIF;
                break;
        case 9:         //ornamentals
                family = FF_ORNAMENT;
                break;
        case 10:        //scripts
                family = FF_SCRIPT;
                break;
                        //11 = reserved
        case 12:        //symbolic
                family = FF_SYMBOL;
                break;
        default:
                family = FF_NON_PORTABLE;
        } 

        return  FA_USEFUL | FA_OUTLINE | family;   
}


/********************************************************************
 *                      mapFontWeight
 ********************************************************************
 * SYNOPSIS:	  Maps the TrueType font weight class to FreeGEOS 
 *                AdjustedWeight.
 * 
 * PARAMETERS:    weightClass           TrueType weight class.
 * 
 * RETURNS:       AdjustedWeight        FreeGEOS AdjustedWeight.
 * 
 * SIDE EFFECTS:  none
 * 
 * STRATEGY:      
 * 
 * REVISION HISTORY:
 *      Date      Name      Description
 *      ----      ----      -----------
 *      21/01/23  JK        Initial Revision
 *******************************************************************/

static AdjustedWeight mapFontWeight( TT_Short weightClass ) 
{
        switch (weightClass / 100)
        {
        case 1:
                return AW_ULTRA_LIGHT;
        case 2:
                return AW_EXTRA_LIGHT;
        case 3:
                return AW_LIGHT;
        case 4:
                return AW_SEMI_LIGHT;
        case 5:
                return AW_MEDIUM;
        case 6:
                return AW_SEMI_BOLD;
        case 7:
                return AW_BOLD;
        case 8:
                return AW_EXTRA_BOLD;
        default:
                return AW_ULTRA_BOLD;
        }
}


/********************************************************************
 *                      mapTextStyle
 ********************************************************************
 * SYNOPSIS:	  Maps the TrueType subfamily to FreeGEOS TextStyle.
 * 
 * PARAMETERS:    subfamily*            String with subfamiliy name. 
 * 
 * RETURNS:       TextStyle             FreeGEOS TextStyle.
 * 
 * SIDE EFFECTS:  none
 * 
 * STRATEGY:      
 * 
 * REVISION HISTORY:
 *      Date      Name      Description
 *      ----      ----      -----------
 *      21/01/23  JK        Initial Revision
 *******************************************************************/

static TextStyle mapTextStyle( const char* subfamily )
{
        if ( strcmp( subfamily, "Regular" ) == 0 )
                return 0x00;
        if ( strcmp( subfamily, "Bold" ) == 0 )
                return TS_BOLD;
        if ( strcmp( subfamily, "Italic" ) == 0 )
                return TS_ITALIC;
        if ( strcmp( subfamily, "Bold Italic" ) == 0 )
                return TS_BOLD | TS_ITALIC;
        if ( strcmp( subfamily, "Oblique" ) == 0 )
                return TS_ITALIC;
        
        /* only Bold Oblique remains */
        return TS_BOLD | TS_ITALIC;
}


/********************************************************************
 *                      getFontID
 ********************************************************************
 * SYNOPSIS:	        If the passed font family is a mapped font, 
 *                      the FontID form geos.ini is returned, otherwise
 *                      we calculate new FontID and return it.
 * 
 * PARAMETERS:          familyName*     Font family name.
 *
 * RETURNS:             FontID          FontID found geos.ini or calculated.
 * 
 * SIDE EFFECTS:  none
 * 
 * STRATEGY:      
 * 
 * REVISION HISTORY:
 *      Date      Name      Description
 *      ----      ----      -----------
 *      21/01/23  JK        Initial Revision
 *******************************************************************/

static FontID getFontID( const char* familyName ) 
{
        FontID  fontID;

        if( !InitFileReadInteger( FONTMAPPING_CATEGORY, familyName, &fontID ) )
                return FM_TRUETYPE || (fontID && 0x0fff);

        return MAKE_FONTID( familyName );
}


/********************************************************************
 *                      getFontIDAvailIndex
 ********************************************************************
 * SYNOPSIS:	        Searches all FontsAvailEntries for the passed 
 *                      FontID and returns its index. If no FontsAvailEntry 
 *                      is found for the FontID, -1 is returned.
 * 
 * PARAMETERS:          fontID          Searched FontID.
 *                      fontInfoBlock   Memory block with font information.
 * 
 * RETURNS:       
 * 
 * SIDE EFFECTS:  none
 * 
 * STRATEGY:      
 * 
 * REVISION HISTORY:
 *      Date      Name      Description
 *      ----      ----      -----------
 *      21/01/23  JK        Initial Revision
 *******************************************************************/
static sword getFontIDAvailIndex( FontID fontID, MemHandle fontInfoBlock )
{
        FontsAvailEntry*  fontsAvailEntrys;
        word   elements;
        sword   element;

        /* set fontsAvailEntrys to first Element after LMemBlockHeader */
        fontsAvailEntrys = ( (byte*)LMemDeref( 
			ConstructOptr(fontInfoBlock, sizeof(LMemBlockHeader))) );
        elements = LMemGetChunkSizePtr( fontsAvailEntrys ) / sizeof( FontsAvailEntry );

        for( element = 0; element < elements; element++ )
                if( fontsAvailEntrys[element].FAE_fontID == fontID )
                        return element;

        return -1;
}


/********************************************************************
 *                      getNameFromNameTable
 ********************************************************************
 * SYNOPSIS:	  Searches the font's name tables for the given NameID 
 *                and returns its content.
 * 
 * PARAMETERS:    name*         Pointer to result string.
 *                face          TrueType face to be searched.
 *                nameID        ID to be searched.
 * 
 * RETURNS:       word          Length of the table entry found.
 * 
 * SIDE EFFECTS:  none
 * 
 * STRATEGY:      
 * 
 * REVISION HISTORY:
 *      Date      Name      Description
 *      ----      ----      -----------
 *      21/01/23  JK        Initial Revision
 *******************************************************************/

static word getNameFromNameTable( char* name, TT_Face face, TT_UShort nameID )
{
        TT_Face_Properties  faceProperties;
        TT_UShort           platformID;
        TT_UShort           encodingID;
        TT_UShort           languageID;
        word                nameLength;
        word                id;
        word                i, n;
        char*               str;
        
        
        TT_Get_Face_Properties( face, &faceProperties );

        for( n = 0; n < faceProperties.num_Names; n++ )
        {
                TT_Get_Name_ID( face, n, &platformID, &encodingID, &languageID, &id );
                if( id != nameID )
                        continue;

                if( platformID == PLATFORM_ID_MS && 
                    encodingID == ENCODING_ID_MS_UNICODE_BMP && 
                    languageID == LANGUAGE_ID_WIN_EN_US )
                {
                        TT_Get_Name_String( face, n, &str, &nameLength );

                        for (i = 1; str != 0 && i < nameLength; i += 2)
                                *name++ = str[i];
                        *name = 0;
                        return nameLength >> 1;
                }
                else if( platformID == PLATFORM_ID_MAC && 
                         encodingID == ENCODING_ID_MAC_ROMAN &&
                         languageID == LANGUAGE_ID_MAC_EN )
                {
                        TT_Get_Name_String( face, n, &str, &nameLength );

                        for (i = 0; str != 0 && i < nameLength; i++)
                                *name++ = str[i];
                        *name = 0;
                        return nameLength;
                }
		else if( encodingID == ENCODING_ID_UNICODE )
		{
			TT_Get_Name_String( face, n, &str, &nameLength );
	
			for (i = 1; str != 0 && i < nameLength; i += 2)
				*name++ = str[i];
			*name = 0;
			return nameLength >> 1;
		}
        }

        return 0;
}


/********************************************************************
 *                      convertHeader
 ********************************************************************
 * SYNOPSIS:	  Converts information from a TrueType font into a 
 *                FreeGEOS FontHeader.
 * 
 * PARAMETERS:    face                  Face from which the information 
 *                                      is to be converted.
 *                faceProperties        FaceProperties to be used.
 *                charMap               CharMap to be used.
 *                instance              Instance to be used.
 *                fontHeader*           Pointer to FontInfo in which the
 *                                      converted information is to be stored.
 * 
 * RETURNS:       void
 * 
 * SIDE EFFECTS:  none
 * 
 * STRATEGY:      
 * 
 * REVISION HISTORY:
 *      Date      Name      Description
 *      ----      ----      -----------
 *      21/01/23  JK        Initial Revision
 *******************************************************************/

static void convertHeader( 
                        TT_Face             face, 
                        TT_Face_Properties  faceProperties, 
                        TT_CharMap          charMap, 
                        FontHeader*         fontHeader )
{
        TT_UShort           charIndex;
        TT_Glyph            glyph;
        TT_Glyph_Metrics    metrics;
        TT_Instance         instance;
        word                geosChar;
        sword               maxAccentOrAscent;
        word                unitsPerEM = faceProperties.header->Units_Per_EM;


        ECCheckBounds( (void*)fontHeader );
        
        /* initialize min, max and avg values in fontHeader */
        fontHeader->FH_minLSB   =  9999;
        fontHeader->FH_maxBSB   = -9999;
        fontHeader->FH_minTSB   = -9999;
        fontHeader->FH_maxRSB   = -9999;
        fontHeader->FH_avgwidth = 0;

        fontHeader->FH_numChars = InitGeosCharsInCharMap( charMap, 
                                                           &fontHeader->FH_firstChar, 
                                                           &fontHeader->FH_lastChar ); 

        TT_New_Glyph( face, &glyph );
        TT_New_Instance( face, &instance );

        for ( geosChar = fontHeader->FH_firstChar; geosChar < fontHeader->FH_lastChar; ++geosChar )
        {
                word unicode = GeosCharToUnicode( geosChar );

                charIndex = TT_Char_Index( charMap, unicode );
                if ( charIndex == 0 )
                        break;

                /* load glyph without scaling or hinting */
                TT_Load_Glyph( instance, glyph, charIndex, TTLOAD_DEFAULT );
                TT_Get_Glyph_Metrics( glyph, &metrics );

                //h_height
                if( unicode == C_LATIN_CAPITAL_LETTER_H )
                        fontHeader->FH_h_height = metrics.bbox.yMax;

                //x_height
                if ( unicode == C_LATIN_SMALL_LETTER_X )
                        fontHeader->FH_x_height = metrics.bbox.yMax;

                //ascender
                if ( unicode == C_LATIN_SMALL_LETTER_D )
                        fontHeader->FH_ascender = metrics.bbox.yMax;

                //descender
                if ( unicode == C_LATIN_SMALL_LETTER_P )
                        fontHeader->FH_descender = metrics.bbox.yMin;

                //width
                if ( fontHeader->FH_maxwidth < ( metrics.bbox.xMax - metrics.bbox.xMin ) )
                        fontHeader->FH_maxwidth = metrics.bbox.xMax - metrics.bbox.xMin;

                //avg width
                if ( GeosAvgWidth( geosChar ) ) 
                {
                        fontHeader->FH_avgwidth = fontHeader->FH_avgwidth + (
                                  ( metrics.bbox.xMax - metrics.bbox.xMin ) * GeosAvgWidth( geosChar ) / 1000 );
                }
                
                /* scan xMin */
                if( fontHeader->FH_minLSB > metrics.bbox.xMin )
                        fontHeader->FH_minLSB = (sword) metrics.bbox.xMin;

                /* scan xMax */
                if ( fontHeader->FH_maxRSB < metrics.bbox.xMax )
                        fontHeader->FH_maxRSB = metrics.bbox.xMax;
                /* scan yMin */
                if ( fontHeader->FH_maxBSB < metrics.bbox.yMin )
                        fontHeader->FH_maxBSB = metrics.bbox.yMin;

                //yMax
                if ( fontHeader->FH_minTSB < metrics.bbox.yMax )
                {
                        fontHeader->FH_minTSB = metrics.bbox.yMax;
                        if ( GeosCharMapFlag( geosChar ) == CMF_ACCENT && 
                             fontHeader->FH_accent < metrics.bbox.yMax )
                             fontHeader->FH_accent = metrics.bbox.yMax;
                }

        }
        TT_Done_Glyph( glyph );
        TT_Done_Instance( instance );

        //baseline
        if ( fontHeader->FH_accent <= 0 )
        {
                fontHeader->FH_accent = 0;
                maxAccentOrAscent = fontHeader->FH_ascent;
        }
        else
        {
                maxAccentOrAscent = fontHeader->FH_accent;
                fontHeader->FH_accent = fontHeader->FH_accent - fontHeader->FH_ascent;
        }
                
        fontHeader->FH_baseAdjust = BASELINE( unitsPerEM )- maxAccentOrAscent;
        fontHeader->FH_height = fontHeader->FH_maxBSB + maxAccentOrAscent;
        fontHeader->FH_minTSB = fontHeader->FH_minTSB - BASELINE( unitsPerEM );
        fontHeader->FH_maxBSB = fontHeader->FH_maxBSB - ( DESCENT( unitsPerEM ) -
                                                          SAFETY( unitsPerEM ) );

        fontHeader->FH_underPos = faceProperties.postscript->underlinePosition;
        if( fontHeader->FH_underPos == 0 )
                fontHeader->FH_underPos = DEFAULT_UNDER_POSITION( unitsPerEM );

        fontHeader->FH_underPos = maxAccentOrAscent - fontHeader->FH_underPos;

        fontHeader->FH_underThick = faceProperties.postscript->underlineThickness; 
        if( fontHeader->FH_underThick == 0 )
                fontHeader->FH_underThick = DEFAULT_UNDER_THICK( unitsPerEM );
        
        if( fontHeader->FH_x_height > 0 )
                fontHeader->FH_strikePos = 3 * fontHeader->FH_x_height / 5;
        else
                fontHeader->FH_strikePos = 3 * fontHeader->FH_ascent / 5;

        fontHeader->FH_continuitySize = DEFAULT_CONTINUITY_CUTOFF( unitsPerEM );
}


/*******************************************************************/
/* We cannot use functions from the Ansic library, which causes a  */
/* cycle. Therefore, the required functions are reimplemented here.*/
/*******************************************************************/

static int strlen( const char* str )
{
        const char  *s;

        for ( s = str; *s; ++s )
                ;
        return( s - str );
}


static void strcpy( char* dest, const char* source )
{
        while ((*dest++ = *source++) != '\0');
}


static int strcmp( const char* s1, const char* s2 )
{
        while ( *s1 && ( *s1 == *s2 ) )
        {
                s1++;
                s2++;
        }
        return *(const unsigned char*)s1 - *(const unsigned char*)s2;
}

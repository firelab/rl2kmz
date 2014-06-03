/******************************************************************************
*
*  Project:  rl2kmz
*  Purpose:  c port of rl2kmz.py, create kmz from rain/lightning grid
*  Author:   Kyle Shannon <kyle at pobox dot com>
*
*******************************************************************************
*
* This is free and unencumbered software released into the public domain.
*
* Anyone is free to copy, modify, publish, use, compile, sell, or
* distribute this software, either in source code form or as a compiled
* binary, for any purpose, commercial or non-commercial, and by any
* means.
*
* In jurisdictions that recognize copyright laws, the author or authors
* of this software dedicate any and all copyright interest in the
* software to the public domain. We make this dedication for the benefit
* of the public at large and to the detriment of our heirs and
* successors. We intend this dedication to be an overt act of
* relinquishment in perpetuity of all present and future rights to this
* software under copyright law.
*
* THIS SOFTWARE WAS DEVELOPED AT THE ROCKY MOUNTAIN RESEARCH STATION (RMRS)
* MISSOULA FIRE SCIENCES LABORATORY BY EMPLOYEES OF THE FEDERAL GOVERNMENT
* IN THE COURSE OF THEIR OFFICIAL DUTIES. PURSUANT TO TITLE 17 SECTION 105
* OF THE UNITED STATES CODE, THIS SOFTWARE IS NOT SUBJECT TO COPYRIGHT
* PROTECTION AND IS IN THE PUBLIC DOMAIN. RMRS MISSOULA FIRE SCIENCES
* LABORATORY ASSUMES NO RESPONSIBILITY WHATSOEVER FOR ITS USE BY OTHER
* PARTIES,  AND MAKES NO GUARANTEES, EXPRESSED OR IMPLIED, ABOUT ITS QUALITY,
* RELIABILITY, OR ANY OTHER CHARACTERISTIC.
*
* THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,
* EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
* MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.
* IN NO EVENT SHALL THE AUTHORS BE LIABLE FOR ANY CLAIM, DAMAGES OR
* OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE,
* ARISING FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR
* OTHER DEALINGS IN THE SOFTWARE.
*
* For more information, please refer to <http://unlicense.org>
*
******************************************************************************/

#include "gdal.h"
#include "gdalwarper.h"
#include "ogr_api.h"
#include "cpl_string.h"
#include "cpl_conv.h"

#ifndef RL_OK
#define RL_OK  0
#endif

#ifndef RL_ERR
#define RL_ERR 1
#endif

#ifndef RL_OGR_STYLE_BLACK_NO_FILL
#define RL_OGR_STYLE_BLACK_NO_FILL "PEN(c:#000000FF,w:2px);BRUSH(fc:#A9A9A9FF)"
#endif
#ifndef RL_OGR_STYLE_RED_NO_FILL
#define RL_OGR_STYLE_RED_NO_FILL   "PEN(c:#FF0000FF,w:1px);BRUSH(fc:#e9967AFF)"
#endif

static char ** ParseConfigFile( const char *pszConfigFile )
{
    char **papszConfig = NULL;
    if( !pszConfigFile )
    {
        CPLError( CE_Failure, CPLE_AppDefined,
                  "Invalid filename passed for config" );
        return NULL;
    }
    papszConfig = CSLLoad2( pszConfigFile, 100, 100, NULL );
    if( !papszConfig )
    {
        CPLError( CE_Failure, CPLE_AppDefined,
                  "Could not read valid config" );
    }
    return papszConfig;
}

static const char * FetchConfigOption( char **papszConfig,
                                       const char *pszKey,
                                       const char *pszDefault )
{
    const char *pszValue;
    pszValue = CSLFetchNameValue( papszConfig, pszKey );
    if( pszValue == NULL )
    {
        if( pszDefault )
        {
            CPLError( CE_Warning, CPLE_AppDefined,
                      "Could not find value in configuration, " \
                      "using default of %s", pszDefault );
            return pszDefault;
        }
        else
        {
            return NULL;
        }
    }
    return pszValue;
}

void Usage()
{
    printf( "Usage: rl2kmz [-c config_file] src_dataset dst_file\n" );
    exit( 1 );
}

int main( int argc, char *argv[] )
{
    int rc;
    /*
    ** All of our gdal datasets, drivers, bands, and layers.
    */
    GDALDatasetH hRainDS, hMemDS, hWarpDS, hPngOutDS;
    GDALDatasetH hKmlIn, hKmlOut, hScratch;
    GDALDriverH hMemDriver, hPngDriver, hLibKmlDriver;
    OGRLayerH hLayerIn, hLayerOut, hOverlayLayer, hSqlLayer;
    OGRFeatureDefnH hFeatDefn;
    GDALRasterBandH hRed, hGreen, hBlue, hAlpha, hBand;
    OGRFeatureH hFeature, hNewFeature;
    OGRFieldDefnH hFieldDefn;
    OGRFieldType eFieldType;

    /*
    ** SRS
    */
    const char *pszSrcWkt, *pszDstWkt;

    int nXSize, nYSize;
    double adfGeoTransform[6];

    /*
    ** Dynamic arrays.
    */
    int *panSrcData;
    char *pabyRed, *pabyGreen, *pabyBlue, *pabyAlpha;
    double dfNoData;
    int i, j;

    GDALWarpOptions *psWarpOptions;

    double dfNorth, dfSouth, dfEast, dfWest;

    const char *pszOption;
    char **papszKmlOptions = NULL;

    const char *pszTmpBuf;

    char **papszConfigOptions = NULL;

    /*
    ** Various file paths
    */
    const char *pszSrcFile = NULL;
    const char *pszDstFile = NULL;
    const char *pszVsiFile = NULL;
    const char *pszLegendFile;
    const char *pszTitleFile;
    const char *pszPolyLegendFile;
    const char *pszPolygonKmlFile;
    const char *pszDateFile;
    const char *pszDateString = NULL;
    const char *pszName;
    const char *pszCriticalStyle;
    const char *pszExtremeStyle;
    const char *pszLayerName;
    const char *pszSql;

    VSILFILE *fin;

    i = 1;
    while( i < argc )
    {
        if( EQUAL( argv[i], "-c" ) || EQUAL( argv[i], "--config" ) )
        {
            if( i+ 1 < argc )
            {
                papszConfigOptions = ParseConfigFile( argv[++i] );
                if( !papszConfigOptions )
                {
                    Usage();
                }
            }
        }
        else if( EQUAL( argv[i], "--help" ) || EQUAL( argv[i], "-h" ) )
        {
            Usage();
        }
        else if( pszSrcFile == NULL )
        {
            pszSrcFile = argv[i];
        }
        else if( pszDstFile == NULL )
        {
            pszDstFile = argv[i];
        }
        i++;
    }
    /* Image file for legend */
    pszLegendFile =
        FetchConfigOption( papszConfigOptions, "dry_ltng_legend",
                           "dry_ltng_legend.png" );
    /* Title image file */
    pszTitleFile =
        FetchConfigOption( papszConfigOptions, "dry_ltng_title",
                           "dry_ltng_title.png" );
    /* Polygon legend image file */
    pszPolyLegendFile =
        FetchConfigOption( papszConfigOptions, "poly_legend",
                           "/fsfiles/office/wfas2/dir-key/critical.png" );
    /* File to look for polygons in */
    pszPolygonKmlFile =
        FetchConfigOption( papszConfigOptions, "poly_kml",
                           "/fsfiles/office/wfas2/dir-kml/spc_day1firewx.kmz" );

    /* Date string file */
    pszDateFile =
        FetchConfigOption( papszConfigOptions, "date_file", NULL );

    if( pszDateFile )
    {
        fin = VSIFOpenL( pszDateFile, "rb" );
        if( fin )
        {
            pszDateString = CPLStrdup( CPLReadLine2L( fin, 100, NULL ) );
            if( !pszDateString || EQUAL( pszDateString, "" ) )
                pszDateString = NULL;
            VSIFCloseL( fin );
        }
        else
            pszDateString = NULL;
    }

    pszExtremeStyle =
        FetchConfigOption( papszConfigOptions, "extreme_style",
                           RL_OGR_STYLE_BLACK_NO_FILL );
    pszCriticalStyle =
        FetchConfigOption( papszConfigOptions, "critical_style",
                           RL_OGR_STYLE_RED_NO_FILL );

    GDALAllRegister();
    /*
    ** Open the input Arc/Info Binary Grid
    */
    hRainDS = GDALOpenEx( pszSrcFile, GDAL_OF_READONLY | GDAL_OF_RASTER |
                          GDAL_OF_VERBOSE_ERROR, NULL, NULL, NULL );
    if( !hRainDS )
        exit( RL_ERR );

    GDALGetGeoTransform( hRainDS, adfGeoTransform );
    pszSrcWkt = GDALGetProjectionRef( hRainDS );

    nXSize = GDALGetRasterXSize( hRainDS );
    nYSize = GDALGetRasterYSize( hRainDS );

    hMemDriver = GDALGetDriverByName( "MEM" );
    hMemDS = GDALCreate( hMemDriver, "/vsimem/rain.mem", 
                         nXSize, nYSize, 4, GDT_Byte, NULL );
    GDALSetGeoTransform( hMemDS, adfGeoTransform );
    GDALSetProjection( hMemDS, pszSrcWkt );

    hRed = GDALGetRasterBand( hMemDS, 1 );
    hGreen = GDALGetRasterBand( hMemDS, 2 );
    hBlue = GDALGetRasterBand( hMemDS, 3 );
    hAlpha = GDALGetRasterBand( hMemDS, 4 );

    panSrcData = (int*) CPLMalloc( sizeof( int ) * nXSize );
    pabyRed= (char*) CPLMalloc( sizeof( char ) * nXSize );
    pabyGreen= (char*) CPLMalloc( sizeof( char ) * nXSize );
    pabyBlue= (char*) CPLMalloc( sizeof( char ) * nXSize );
    pabyAlpha= (char*) CPLMalloc( sizeof( char ) * nXSize );
    //read in source data
    hBand = GDALGetRasterBand( hRainDS, 1 );
    dfNoData = GDALGetRasterNoDataValue( hBand, &rc );

    for( i = 0;i < nYSize;i++ )
    {
        rc = GDALRasterIO( hBand, GF_Read, 0, i, nXSize, 1,
                           panSrcData, nXSize, 1, GDT_Int32, 0, 0 );

        /*********************************************************************/
        /* Scale data using a remap from paul. Color table?                  */
        /*********************************************************************/
        /*                                                                   */ 
        /* -32768 0 0 0 0 src_nodata                                         */
        /* 0 -> 0 0 0 0 empty                                                */
        /* 1-10 -> 204 191 102 255                                           */
        /* 11-25 -> 230 176 0 255                                            */
        /* 26-50 -> 255 255 130 255                                          */
        /* 51-91 -> 163 230 0 255                                            */
        /* 92-100 -> 112 187 0 255                                           */
        /* 101-5000 -> 112 188 0 255                                         */
        /* 10000 -> 0 0 255 255 (negative strike)                            */
        /* 20000 -> 255 0 0 255 (positive strike)                            */
        /*                                                                   */
        /*********************************************************************/
        for( j = 0;j < nXSize;j++ )
        {
            if( panSrcData[j] == dfNoData )
            {
                pabyRed[j] = 0;
                pabyGreen[j] = 0;
                pabyBlue[j] = 0;
                pabyAlpha[j] = 0;
            }
            else if( panSrcData[j] == 0 )
            {
                pabyRed[j] = 0;
                pabyGreen[j] = 0;
                pabyBlue[j] = 0;
                pabyAlpha[j] = 0;
            }
            else if( panSrcData[j] >= 1 && panSrcData[j] <= 10 )
            {
                pabyRed[j] = 204;
                pabyGreen[j] = 191;
                pabyBlue[j] = 102;
                pabyAlpha[j] = 255;
            }
            else if( panSrcData[j] >= 11 && panSrcData[j] <= 25 )
            {
                pabyRed[j] = 230;
                pabyGreen[j] = 176;
                pabyBlue[j] = 0;
                pabyAlpha[j] = 255;
            }
            else if( panSrcData[j] >= 26 && panSrcData[j] <= 50 )
            { 
                pabyRed[j] = 255;
                pabyGreen[j] = 255;
                pabyBlue[j] = 130;
                pabyAlpha[j] = 255;
            }
            else if( panSrcData[j] >= 51 && panSrcData[j] <= 91 )
            {
                pabyRed[j] = 163;
                pabyGreen[j] = 230;
                pabyBlue[j] = 0;
                pabyAlpha[j] = 255;
            }

            else if( panSrcData[j] >= 92 && panSrcData[j] <= 100 )
            {
                pabyRed[j] = 112;
                pabyGreen[j] = 187;
                pabyBlue[j] = 0;
                pabyAlpha[j] = 255;
            }
            else if( panSrcData[j] >= 101 && panSrcData[j] <= 5000 )
            {
                pabyRed[j] = 112;
                pabyGreen[j] = 188;
                pabyBlue[j] = 0;
                pabyAlpha[j] = 255;
            }
            else if( panSrcData[j] == 10000 )
            {
                pabyRed[j] = 0;
                pabyGreen[j] = 0;
                pabyBlue[j] = 255;
                pabyAlpha[j] = 255;
            }
            else if( panSrcData[j] == 20000 )
            {
                pabyRed[j] = 255;
                pabyGreen[j] = 0;
                pabyBlue[j] = 0;
                pabyAlpha[j] = 255;
            }
        }
        rc = GDALRasterIO( hRed, GF_Write, 0, i, nXSize, 1, pabyRed,
                           nXSize, 1, GDT_Byte, 0, 0 );
        rc = GDALRasterIO( hGreen, GF_Write, 0, i, nXSize, 1, pabyGreen,
                           nXSize, 1, GDT_Byte, 0, 0 );
        rc = GDALRasterIO( hBlue, GF_Write, 0, i, nXSize, 1, pabyBlue,
                           nXSize, 1, GDT_Byte, 0, 0 );
        rc = GDALRasterIO( hAlpha, GF_Write, 0, i, nXSize, 1, pabyAlpha,
                           nXSize, 1, GDT_Byte, 0, 0 );
    }
    CPLFree( panSrcData );
    CPLFree( pabyRed );
    CPLFree( pabyGreen );
    CPLFree( pabyBlue );
    CPLFree( pabyAlpha );

    pszSrcWkt = "PROJCS[\"unnamed\",GEOGCS[\"unnamed ellipse" \
                "\",DATUM[\"unknown\",SPHEROID[\"unnamed\"," \
                "6370997,0]],PRIMEM[\"Greenwich\",0],UNIT[\"" \
                "degree\",0.0174532925199433]],PROJECTION[\"" \
                "Lambert_Azimuthal_Equal_Area\"]," \
                "PARAMETER[\"latitude_of_center\",45]," \
                "PARAMETER[\"longitude_of_center\",-100]," \
                "PARAMETER[\"false_easting\",0]," \
                "PARAMETER[\"false_northing\",0],UNIT[\"Meter\",1]]";
    pszDstWkt = "GEOGCS[\"WGS 84\",DATUM[\"WGS_1984\"," \
                "SPHEROID[\"WGS 84\",6378137,298.257223563," \
                "AUTHORITY[\"EPSG\",\"7030\"]]," \
                "AUTHORITY[\"EPSG\",\"6326\"]]," \
                "PRIMEM[\"Greenwich\",0," \
                "AUTHORITY[\"EPSG\",\"8901\"]]," \
                "UNIT[\"degree\",0.01745329251994328," \
                "AUTHORITY[\"EPSG\",\"9122\"]]," \
                "AUTHORITY[\"EPSG\",\"4326\"]]";

    psWarpOptions = GDALCreateWarpOptions();

    hWarpDS = GDALAutoCreateWarpedVRT( hMemDS, pszSrcWkt, pszDstWkt,
                                       GRA_NearestNeighbour, 0.0,
                                       psWarpOptions );

    GDALDestroyWarpOptions( psWarpOptions );
    /*
    ** Grab info for our bounding box in the ground overlay before we kill the
    ** warped dataset.
    */
    GDALGetGeoTransform( hWarpDS, adfGeoTransform );
    nXSize = GDALGetRasterXSize( hWarpDS );
    nYSize = GDALGetRasterYSize( hWarpDS );
    dfNorth = adfGeoTransform[3] +
              adfGeoTransform[4] * 0 +
              adfGeoTransform[5] * 0;
    dfEast = adfGeoTransform[0] +
             adfGeoTransform[1] * nXSize +
             adfGeoTransform[2] * 0;
    dfSouth = adfGeoTransform[3] +
              adfGeoTransform[4] * 0 +
              adfGeoTransform[5] * nYSize;
    dfWest = adfGeoTransform[0] +
             adfGeoTransform[1] * 0 +
             adfGeoTransform[2] * 0;

    hKmlIn = OGROpen( pszPolygonKmlFile, FALSE, NULL );
    hLibKmlDriver = GDALGetDriverByName( "LIBKML" );
    hKmlOut = OGR_Dr_CreateDataSource( hLibKmlDriver, pszDstFile, NULL );

    hLayerIn = GDALDatasetGetLayer( hKmlIn, 0 );
    pszLayerName = OGR_L_GetName( hLayerIn );

    pszSql = CPLSPrintf( "SELECT * FROM '%s' WHERE Name!='Elevated' " \
                         "ORDER BY Name ASC", pszLayerName );
    hSqlLayer = GDALDatasetExecuteSQL( hKmlIn, pszSql, NULL, NULL );

    /*
    ** We essentially just copy the layer, but we may need some style info to
    ** change and this is restrictive in libkml.  In order to work around this,
    ** create the layer by hand and copy features using CreateFeature.
    */
    hFeatDefn = OGR_L_GetLayerDefn( hSqlLayer );
    pszOption = pszDateString ? pszDateString : OGR_L_GetName( hLayerIn );
    pszOption = CPLSPrintf( "NAME=%s", pszOption );
    papszKmlOptions = CSLAddString( papszKmlOptions, pszOption );
    hLayerOut = OGR_DS_CreateLayer( hKmlOut, pszDateString ? pszDateString : OGR_L_GetName( hLayerIn ),
                                    OGR_L_GetSpatialRef( hSqlLayer ),
                                    OGR_L_GetGeomType( hSqlLayer ),
                                    papszKmlOptions );
    CSLDestroy( papszKmlOptions );
    papszKmlOptions = NULL;

    OGR_L_ResetReading( hSqlLayer );
    while( ( hFeature = OGR_L_GetNextFeature( hSqlLayer ) ) != NULL )
    {
        hNewFeature = OGR_F_Create( hFeatDefn );
        for( i = 0; i < OGR_FD_GetFieldCount( hFeatDefn ); i++ )
        {
            hFieldDefn = OGR_FD_GetFieldDefn( hFeatDefn, i );
            eFieldType = OGR_Fld_GetType( hFieldDefn );
            if( eFieldType == OFTDate || eFieldType == OFTDateTime ||
                eFieldType == OFTTime || eFieldType == OFTString )
            {
                OGR_F_SetFieldString( hNewFeature, i,
                                      OGR_F_GetFieldAsString( hFeature, i ) );
            }
            else if( eFieldType == OFTInteger )
            {
                OGR_F_SetFieldInteger( hNewFeature, i,
                                       OGR_F_GetFieldAsInteger( hFeature, i ) );
            }
            else if( eFieldType == OFTReal )
            {
                OGR_F_SetFieldDouble( hNewFeature, i,
                                      OGR_F_GetFieldAsDouble( hFeature, i ) );
            }
        }

        rc = OGR_F_GetFieldIndex( hNewFeature, "Name" );
        pszName = OGR_F_GetFieldAsString( hNewFeature, rc );
        if( rc >= 0 )
        {
            if( EQUAL( pszName, "Extreme" ) )
            {
                OGR_F_SetStyleString( hNewFeature, pszExtremeStyle );
            }
            else if( EQUAL( pszName, "Critical" ) )
            {
                OGR_F_SetStyleString( hNewFeature, pszCriticalStyle );
            }
        }
        OGR_F_SetGeometry( hNewFeature, OGR_F_GetGeometryRef( hFeature ) );
        OGR_L_CreateFeature( hLayerOut, hNewFeature );
        OGR_F_Destroy( hFeature );
        OGR_F_Destroy( hNewFeature );
    }

    /*
    ** Add the ground overlay data.
    */
    papszKmlOptions = CSLAddString( papszKmlOptions, "GO_HREF=rainandlightning.png" );
    papszKmlOptions = CSLAddString( papszKmlOptions, "GO_NAME=rainandltng" );
    papszKmlOptions = CSLAddString( papszKmlOptions, "NAME=rainandltng" );
    papszKmlOptions = CSLAddString( papszKmlOptions, "GO_DESCRIPTION=rainandltng" );
    pszOption = CPLSPrintf( "GO_NORTH=%lf", dfNorth );
    papszKmlOptions = CSLAddString( papszKmlOptions, pszOption );
    pszOption = CPLSPrintf( "GO_SOUTH=%lf", dfSouth );
    papszKmlOptions = CSLAddString( papszKmlOptions, pszOption );
    pszOption = CPLSPrintf( "GO_EAST=%lf", dfEast );
    papszKmlOptions = CSLAddString( papszKmlOptions, pszOption );
    pszOption = CPLSPrintf( "GO_WEST=%lf", dfWest );
    papszKmlOptions = CSLAddString( papszKmlOptions, pszOption );

    hOverlayLayer = GDALDatasetCreateLayer( hKmlOut, "raster", NULL,
                                            wkbUnknown, papszKmlOptions );
    CSLDestroy( papszKmlOptions );
    papszKmlOptions = NULL;
    /*
    ** Add the legend for dry lightning.
    */
    pszOption = CPLSPrintf( "SO_HREF=%s",
                            CPLGetFilename( pszTitleFile ) );
    papszKmlOptions = CSLAddString( papszKmlOptions, pszOption );
    papszKmlOptions = CSLAddString( papszKmlOptions, "SO_NAME=title" );
    pszOption = CPLSPrintf( "NAME=%s", CPLGetBasename( pszTitleFile ) );
    papszKmlOptions = CSLAddString( papszKmlOptions, pszOption );
    papszKmlOptions = CSLAddString( papszKmlOptions, "SO_OVERLAY_X=0.5" );
    papszKmlOptions = CSLAddString( papszKmlOptions, "SO_OVERLAY_Y=1.0" );
    papszKmlOptions = CSLAddString( papszKmlOptions, "SO_OVERLAY_XUNITS=fraction" );
    papszKmlOptions = CSLAddString( papszKmlOptions, "SO_OVERLAY_YUNITS=fraction" );
    papszKmlOptions = CSLAddString( papszKmlOptions, "SO_SCREEN_X=0.5" );
    papszKmlOptions = CSLAddString( papszKmlOptions, "SO_SCREEN_Y=1.0" );
    papszKmlOptions = CSLAddString( papszKmlOptions, "SO_SCREEN_XUNITS=fraction" );
    papszKmlOptions = CSLAddString( papszKmlOptions, "SO_SCREEN_YUNITS=fraction" );
    papszKmlOptions = CSLAddString( papszKmlOptions, "SO_SIZE_X=-1.0" );
    papszKmlOptions = CSLAddString( papszKmlOptions, "SO_SIZE_Y=-1.0" );
    papszKmlOptions = CSLAddString( papszKmlOptions, "SO_SIZE_XUNITS=fraction" );
    papszKmlOptions = CSLAddString( papszKmlOptions, "SO_SIZE_YUNITS=fraction" );

    hOverlayLayer = GDALDatasetCreateLayer( hKmlOut, "title", NULL,
                                            wkbUnknown, papszKmlOptions );
    CSLDestroy( papszKmlOptions );
    papszKmlOptions = NULL;

    pszOption = CPLSPrintf( "SO_HREF=%s",
                            CPLGetFilename( pszLegendFile ) );
    papszKmlOptions = CSLAddString( papszKmlOptions, pszOption );
    papszKmlOptions = CSLAddString( papszKmlOptions, "SO_NAME=legend" );
    pszOption = CPLSPrintf( "NAME=%s", CPLGetBasename( pszLegendFile ) );
    papszKmlOptions = CSLAddString( papszKmlOptions, pszOption );
    papszKmlOptions = CSLAddString( papszKmlOptions, "SO_OVERLAY_X=0.0" );
    papszKmlOptions = CSLAddString( papszKmlOptions, "SO_OVERLAY_Y=0.0" );
    papszKmlOptions = CSLAddString( papszKmlOptions, "SO_OVERLAY_XUNITS=fraction" );
    papszKmlOptions = CSLAddString( papszKmlOptions, "SO_OVERLAY_YUNITS=fraction" );
    papszKmlOptions = CSLAddString( papszKmlOptions, "SO_SCREEN_X=0.0" );
    papszKmlOptions = CSLAddString( papszKmlOptions, "SO_SCREEN_Y=0.0" );
    papszKmlOptions = CSLAddString( papszKmlOptions, "SO_SCREEN_XUNITS=fraction" );
    papszKmlOptions = CSLAddString( papszKmlOptions, "SO_SCREEN_YUNITS=fraction" );
    papszKmlOptions = CSLAddString( papszKmlOptions, "SO_SIZE_X=0.25" );
    papszKmlOptions = CSLAddString( papszKmlOptions, "SO_SIZE_Y=0.25" );
    papszKmlOptions = CSLAddString( papszKmlOptions, "SO_SIZE_XUNITS=fraction" );
    papszKmlOptions = CSLAddString( papszKmlOptions, "SO_SIZE_YUNITS=fraction" );

    hOverlayLayer = GDALDatasetCreateLayer( hKmlOut, "legend", NULL,
                                            wkbUnknown, papszKmlOptions );
    CSLDestroy( papszKmlOptions );
    papszKmlOptions = NULL;

    pszOption = CPLSPrintf( "SO_HREF=%s",
                            CPLGetFilename( pszPolyLegendFile ) );
    papszKmlOptions = CSLAddString( papszKmlOptions, pszOption );
    papszKmlOptions = CSLAddString( papszKmlOptions, "SO_NAME=lgtng_legend" );
    pszOption = CPLSPrintf( "NAME=%s", CPLGetBasename( pszPolyLegendFile ) );
    papszKmlOptions = CSLAddString( papszKmlOptions, pszOption );
    papszKmlOptions = CSLAddString( papszKmlOptions, "SO_OVERLAY_X=1.0" );
    papszKmlOptions = CSLAddString( papszKmlOptions, "SO_OVERLAY_Y=0.0" );
    papszKmlOptions = CSLAddString( papszKmlOptions, "SO_OVERLAY_XUNITS=fraction" );
    papszKmlOptions = CSLAddString( papszKmlOptions, "SO_OVERLAY_YUNITS=fraction" );
    papszKmlOptions = CSLAddString( papszKmlOptions, "SO_SCREEN_X=1.0" );
    papszKmlOptions = CSLAddString( papszKmlOptions, "SO_SCREEN_Y=0.0" );
    papszKmlOptions = CSLAddString( papszKmlOptions, "SO_SCREEN_XUNITS=fraction" );
    papszKmlOptions = CSLAddString( papszKmlOptions, "SO_SCREEN_YUNITS=fraction" );

    hOverlayLayer = GDALDatasetCreateLayer( hKmlOut, "lgtng_legend", NULL,
                                            wkbUnknown, papszKmlOptions );
    CSLDestroy( papszKmlOptions );
    papszKmlOptions = NULL;

    /*
    ** Free this up so we can write the png to it using vsizip.
    */
    GDALDatasetReleaseResultSet( hKmlIn, hSqlLayer );
    GDALClose( hKmlOut ); hKmlOut = NULL;

    pszVsiFile = CPLSPrintf( "/vsizip/%s/layers", pszDstFile );
    hPngDriver = GDALGetDriverByName( "PNG" );
    /*
    ** Supress .aux.xml creation.
    */
    CPLSetConfigOption( "GDAL_PAM_ENABLED", "OFF" );
    /* The rain grid we created */
    pszTmpBuf = CPLSPrintf( "%s/rainandlightning.png", pszVsiFile );
    hPngOutDS = GDALCreateCopy( hPngDriver, pszTmpBuf, hWarpDS, FALSE,
                                NULL, NULL, NULL );
    GDALClose( hPngOutDS ); hPngOutDS = NULL;

    /* The title */
    hScratch = GDALOpenEx( pszTitleFile, GDAL_OF_READONLY | GDAL_OF_RASTER,
                           NULL, NULL, NULL );
    pszTmpBuf =  CPLSPrintf( "%s/%s", pszVsiFile,
                             CPLGetFilename( pszTitleFile ) );
    hPngOutDS = GDALCreateCopy( hPngDriver, pszTmpBuf, hScratch, FALSE,
                                NULL, NULL, NULL );
    GDALClose( hPngOutDS ); hPngOutDS = NULL;
    GDALClose( hScratch ); hScratch = NULL;

    /* The legend for the dry lightning */
    hScratch = GDALOpenEx( pszLegendFile, GDAL_OF_READONLY | GDAL_OF_RASTER,
                           NULL, NULL, NULL );
    pszTmpBuf = CPLSPrintf( "%s/%s", pszVsiFile,
                            CPLGetFilename( pszLegendFile ) );
    hPngOutDS = GDALCreateCopy( hPngDriver, pszTmpBuf, hScratch, FALSE,
                                NULL, NULL, NULL );
    GDALClose( hPngOutDS ); hPngOutDS = NULL;
    GDALClose( hScratch ); hScratch = NULL;

    /* The legend for the polygons */
    hScratch = GDALOpenEx( pszPolyLegendFile, GDAL_OF_READONLY | GDAL_OF_RASTER,
                           NULL, NULL, NULL );
    pszTmpBuf = CPLSPrintf( "%s/%s", pszVsiFile,
                            CPLGetFilename( pszPolyLegendFile ) );
    hPngOutDS = GDALCreateCopy( hPngDriver, pszTmpBuf, hScratch, FALSE,
                                NULL, NULL, NULL );
    GDALClose( hPngOutDS ); hPngOutDS = NULL;
    GDALClose( hScratch ); hScratch = NULL;

    CPLSetConfigOption( "GDAL_PAM_ENABLED", "ON" );

    CSLDestroy( papszConfigOptions );
    GDALClose( hRainDS );
    GDALClose( hWarpDS );
    GDALClose( hMemDS );
    GDALClose( hKmlIn );
    CPLFree( (void*)pszDateString );

    return RL_OK;
}

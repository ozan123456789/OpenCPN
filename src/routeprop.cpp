/**************************************************************************
*
* Project:  OpenCPN
* Purpose:  RouteProperties Support
* Author:   David Register
*
***************************************************************************
*   Copyright (C) 2010 by David S. Register                               *
*                                                                         *
*   This program is free software; you can redistribute it and/or modify  *
*   it under the terms of the GNU General Public License as published by  *
*   the Free Software Foundation; either version 2 of the License, or     *
*   (at your option) any later version.                                   *
*                                                                         *
*   This program is distributed in the hope that it will be useful,       *
*   but WITHOUT ANY WARRANTY; without even the implied warranty of        *
*   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the         *
*   GNU General Public License for more details.                          *
*                                                                         *
*   You should have received a copy of the GNU General Public License     *
*   along with this program; if not, write to the                         *
*   Free Software Foundation, Inc.,                                       *
*   51 Franklin Street, Fifth Floor, Boston, MA 02110-1301,  USA.         *
**************************************************************************/

// For compilers that support precompilation, includes "wx/wx.h".
#include "wx/wxprec.h"

#ifndef WX_PRECOMP
#include "wx/wx.h"
#endif

#include <wx/datetime.h>
#include <wx/clipbrd.h>
#include <wx/print.h>
#include <wx/printdlg.h>
#include <wx/stattext.h>
#include <wx/clrpicker.h>
#include <wx/bmpbuttn.h>

#include "styles.h"
#include "routeprop.h"
#include "MarkInfo.h"
#include "navutil.h"                // for Route
#include "georef.h"
#include "chart1.h"
#include "routeman.h"
#include "routemanagerdialog.h"
#include "routeprintout.h"
#include "chcanv.h"
#include "tcmgr.h"        // pjotrc 2011.03.02
#include "PositionParser.h"
#include "pluginmanager.h"
#include "OCPNPlatform.h"
#include "Route.h"
#include "chart1.h"

#ifdef __OCPN__ANDROID__
#include "androidUTIL.h"
#include <QtWidgets/QScroller>
#endif


extern double             gLat, gLon, gSog, gCog;
extern double             g_PlanSpeed;
extern wxDateTime         g_StartTime;
extern int                g_StartTimeTZ;
extern IDX_entry          *gpIDX;
extern TCMgr              *ptcmgr;
extern long               gStart_LMT_Offset;
extern MyConfig           *pConfig;
extern WayPointman        *pWayPointMan;
extern Select             *pSelect;
extern Routeman           *g_pRouteMan;
extern RouteManagerDialog *pRouteManagerDialog;
extern RouteProp          *pRoutePropDialog;
extern Track              *g_pActiveTrack;
extern RouteList          *pRouteList;
extern PlugInManager      *g_pi_manager;
extern bool                g_bShowTrue, g_bShowMag;

extern MyFrame            *gFrame;
extern OCPNPlatform       *g_Platform;
extern wxString           g_default_wp_icon;

// Global print data, to remember settings during the session
extern wxPrintData               *g_printData;

// Global page setup data
extern wxPageSetupData*          g_pageSetupData;

extern float              g_ChartScaleFactorExp;
extern int                g_GUIScaleFactor;
extern int                g_iDistanceFormat;
extern int                g_iSpeedFormat;
extern MarkInfoDlg       *g_pMarkInfoDialog;
extern double             g_n_arrival_circle_radius;
extern int                g_iWaypointRangeRingsNumber;
extern float              g_fWaypointRangeRingsStep;
extern int                g_iWaypointRangeRingsStepUnits;
extern wxColour           g_colourWaypointRangeRingsColour;

/*!
* Helper stuff for calculating Route Plans
*/

#define    pi        (4.*atan(1.0))
#define    tpi        (2.*pi)
#define    twopi    (2.*pi)
#define    degs    (180./pi)
#define    rads    (pi/180.)

#define    MOTWILIGHT    1    // in some languages there may be a distinction between morning/evening
#define    SUNRISE        2
#define    DAY            3
#define    SUNSET        4
#define    EVTWILIGHT    5
#define    NIGHT        6

/* Next high tide, low tide, transition of the mark level, or some
combination.
Bit      Meaning
0       low tide
1       high tide
2       falling transition
3       rising transition
*/

#define    LW    1
#define    HW    2
#define    FALLING    4
#define    RISING    8

char tide_status[][8] = {
    " LW ",
    " HW ",
    " ~~v ",
    " ~~^ "

};

//  Helper utilities
static wxBitmap LoadSVG( const wxString filename, unsigned int width, unsigned int height )
{
    #ifdef ocpnUSE_SVG
    wxSVGDocument svgDoc;
    if( svgDoc.Load(filename) )
        return wxBitmap( svgDoc.Render( width, height, NULL, true, true ) );
    else
        return wxBitmap(width, height);
    #else
        return wxBitmap(width, height);
    #endif // ocpnUSE_SVG
}



// Sunrise/twilight calculation for route properties.
// limitations: latitude below 60, year between 2000 and 2100
// riset is +1 for rise -1 for set
// adapted by author's permission from QBASIC source as published at
//     http://www.stargazing.net/kepler

wxString GetDaylightString(int index)
{
    switch (index)
    {
        case 0:
            return      _T(" - ");
        case 1:
            return      _("MoTwilight");
        case 2:
            return      _("Sunrise");
        case 3:
            return      _("Daytime");
        case 4:
            return      _("Sunset");
        case 5:
            return      _("EvTwilight");
        case 6:
            return      _("Nighttime");

        default:
            return      _T("");
    }
}


static double sign( double x )
{
    if( x < 0. ) return -1.;
    else
        return 1.;
}

static double FNipart( double x )
{
    return ( sign( x ) * (int) ( fabs( x ) ) );
}

static double FNday( int y, int m, int d, int h )
{
    long fd = ( 367 * y - 7 * ( y + ( m + 9 ) / 12 ) / 4 + 275 * m / 9 + d );
    return ( (double) fd - 730531.5 + h / 24. );
}

static double FNrange( double x )
{
    double b = x / tpi;
    double a = tpi * ( b - FNipart( b ) );
    if( a < 0. ) a = tpi + a;
    return ( a );
}

double getDaylightEvent( double glat, double glong, int riset, double altitude, int y, int m,
        int d )
{
    double day = FNday( y, m, d, 0 );
    double days, correction;
    double utold = pi;
    double utnew = 0.;
    double sinalt = sin( altitude * rads ); // go for the sunrise/sunset altitude first
    double sinphi = sin( glat * rads );
    double cosphi = cos( glat * rads );
    double g = glong * rads;
    double t, L, G, ec, lambda, E, obl, delta, GHA, cosc;
    int limit = 12;
    while( ( fabs( utold - utnew ) > .001 ) ) {
        if( limit-- <= 0 ) return ( -1. );
        days = day + utnew / tpi;
        t = days / 36525.;
        //     get arguments of Sun's orbit
        L = FNrange( 4.8949504201433 + 628.331969753199 * t );
        G = FNrange( 6.2400408 + 628.3019501 * t );
        ec = .033423 * sin( G ) + .00034907 * sin( 2 * G );
        lambda = L + ec;
        E = -1. * ec + .0430398 * sin( 2 * lambda ) - .00092502 * sin( 4. * lambda );
        obl = .409093 - .0002269 * t;
        delta = asin( sin( obl ) * sin( lambda ) );
        GHA = utold - pi + E;
        cosc = ( sinalt - sinphi * sin( delta ) ) / ( cosphi * cos( delta ) );
        if( cosc > 1. ) correction = 0.;
        else
            if( cosc < -1. ) correction = pi;
            else
                correction = acos( cosc );
        double tmp = utnew;
        utnew = FNrange( utold - ( GHA + g + riset * correction ) );
        utold = tmp;
    }
    return ( utnew * degs / 15. );    // returns decimal hours UTC
}

static double getLMT( double ut, double lon )
{
    double t = ut + lon / 15.;
    if( t >= 0. ) if( t <= 24. ) return ( t );
    else
        return ( t - 24. );
    else
        return ( t + 24. );
}

int getDaylightStatus( double lat, double lon, wxDateTime utcDateTime )
{
    if( fabs( lat ) > 60. ) return ( 0 );
    int y = utcDateTime.GetYear();
    int m = utcDateTime.GetMonth() + 1;  // wxBug? months seem to run 0..11 ?
    int d = utcDateTime.GetDay();
    int h = utcDateTime.GetHour();
    int n = utcDateTime.GetMinute();
    int s = utcDateTime.GetSecond();
    if( y < 2000 || y > 2100 ) return ( 0 );

    double ut = (double) h + (double) n / 60. + (double) s / 3600.;
    double lt = getLMT( ut, lon );
    double rsalt = -0.833;
    double twalt = -12.;

    //wxString msg;

    if( lt <= 12. ) {
        double sunrise = getDaylightEvent( lat, lon, +1, rsalt, y, m, d );
        if( sunrise < 0. ) return ( 0 );
        else
            sunrise = getLMT( sunrise, lon );

        //            msg.Printf(_T("getDaylightEvent lat=%f lon=%f\n riset=%d rsalt=%f\n y=%d m=%d d=%d\n sun=%f lt=%f\n ut=%f\n"),
        // lat, lon, +1, rsalt, y, m, d, sunrise, lt, ut);
        //msg.Append(utcDateTime.Format());
        //            OCPNMessageDialog md1(gFrame, msg, _("Sunrise Message"), wxICON_ERROR );
        //            md1.ShowModal();

        if( fabs( lt - sunrise ) < 0.15 ) return ( SUNRISE );
        if( lt > sunrise ) return ( DAY );
        double twilight = getDaylightEvent( lat, lon, +1, twalt, y, m, d );
        if( twilight < 0. ) return ( 0 );
        else
            twilight = getLMT( twilight, lon );
        if( lt > twilight ) return ( MOTWILIGHT );
        else
            return ( NIGHT );
    } else {
        double sunset = getDaylightEvent( lat, lon, -1, rsalt, y, m, d );
        if( sunset < 0. ) return ( 0 );
        else
            sunset = getLMT( sunset, lon );
        if( fabs( lt - sunset ) < 0.15 ) return ( SUNSET );
        if( lt < sunset ) return ( DAY );
        double twilight = getDaylightEvent( lat, lon, -1, twalt, y, m, d );
        if( twilight < 0. ) return ( 0 );
        else
            twilight = getLMT( twilight, lon );
        if( lt < twilight ) return ( EVTWILIGHT );
        else
            return ( NIGHT );
    }
}

#define    UTCINPUT         0
#define    LTINPUT          1    // i.e. this PC local time
#define    LMTINPUT         2    // i.e. the remote location LMT time
#define    INPUT_FORMAT     1
#define    DISPLAY_FORMAT   2
#define    TIMESTAMP_FORMAT 3

wxString ts2s(wxDateTime ts, int tz_selection, long LMT_offset, int format)
{
    wxString s = _T("");
    wxString f;
    if (format == INPUT_FORMAT) f = _T("%m/%d/%Y %H:%M");
    else if (format == TIMESTAMP_FORMAT) f = _T("%m/%d/%Y %H:%M:%S");
    else f = _T(" %m/%d %H:%M");
    switch (tz_selection) {
    case 0: s.Append(ts.Format(f));
        if (format != INPUT_FORMAT) s.Append(_T(" UT"));
        break;
    case 1: s.Append(ts.FromUTC().Format(f)); break;
    case 2:
        wxTimeSpan lmt(0,0,(int)LMT_offset,0);
        s.Append(ts.Add(lmt).Format(f));
        if (format != INPUT_FORMAT) s.Append(_T(" LMT"));
    }
    return(s);
}

/*!
 * RouteProp type definition
 */

IMPLEMENT_DYNAMIC_CLASS( RouteProp, wxDialog )
/*!
 * RouteProp event table definition
 */

BEGIN_EVENT_TABLE( RouteProp, wxDialog )
    EVT_TEXT( ID_PLANSPEEDCTL, RouteProp::OnPlanSpeedCtlUpdated )
    EVT_TEXT_ENTER( ID_STARTTIMECTL, RouteProp::OnStartTimeCtlUpdated )
    EVT_RADIOBOX ( ID_TIMEZONESEL, RouteProp::OnTimeZoneSelected )
    EVT_BUTTON( ID_ROUTEPROP_CANCEL, RouteProp::OnRoutepropCancelClick )
    EVT_BUTTON( ID_ROUTEPROP_OK, RouteProp::OnRoutepropOkClick )
    EVT_LIST_ITEM_SELECTED( ID_LISTCTRL, RouteProp::OnRoutepropListClick )
    EVT_BUTTON( ID_ROUTEPROP_SPLIT, RouteProp::OnRoutepropSplitClick )
    EVT_BUTTON( ID_ROUTEPROP_EXTEND, RouteProp::OnRoutepropExtendClick )
    EVT_BUTTON( ID_ROUTEPROP_PRINT, RouteProp::OnRoutepropPrintClick )
END_EVENT_TABLE()

/*!
 * RouteProp constructors
 */

bool RouteProp::instanceFlag = false;
RouteProp* RouteProp::single = NULL;
RouteProp* RouteProp::getInstance( wxWindow* parent, wxWindowID id, const wxString& title, const wxPoint& pos, const wxSize& size, long style )
{
    if(! instanceFlag)
    {
        single = new RouteProp( parent, id, title, pos, size, style);
        instanceFlag = true;
        return single;
    }
    else
    {
        return single;
    }
}

RouteProp::RouteProp()
{
}

RouteProp::RouteProp( wxWindow* parent, wxWindowID id, const wxString& caption, const wxPoint& pos,
        const wxSize& size, long style )
{
    m_TotalDistCtl = NULL;
    m_wpList = NULL;
    m_nSelected = 0;
    m_pHead = NULL;
    m_pTail = NULL;
    m_pEnroutePoint = NULL;
    m_bStartNow = false;

    m_pRoute = 0;
    m_pEnroutePoint = NULL;
    m_bStartNow = false;
    long wstyle = style;
#ifdef __WXOSX__
    wstyle |= wxSTAY_ON_TOP;
#endif

    SetExtraStyle( GetExtraStyle() | wxWS_EX_BLOCK_EVENTS );
    wxDialog::Create( parent, id, caption, pos, size, style );

    wxFont *qFont = GetOCPNScaledFont(_("Dialog"));
    SetFont( *qFont );
        
    m_bcompact = false;
    for(int i=0; i<COL_COUNT; i++)
        cols[i] = -1;
    
#ifdef __OCPN__ANDROID__
    m_bcompact = true;
    CreateControlsCompact();
#else
    CreateControls();
#endif
    
    RecalculateSize();
    Centre();
}

void RouteProp::RecalculateSize( void )
{
    //  Make an estimate of the dialog size, without scrollbars showing
    
    wxSize esize;
    esize.x = GetCharWidth() * 110;
    esize.y = GetCharHeight() * 44;
    
    wxApp *app = wxTheApp;
    wxWindow *frame = app->GetTopWindow();   // or GetOCPNCanvasWindow()->GetParent();
    if(!frame)
        return;

    
    wxSize dsize = frame->GetClientSize();
    esize.y = wxMin(esize.y, dsize.y - (2 * GetCharHeight()));
    esize.x = wxMin(esize.x, dsize.x - (1 * GetCharHeight()));
    SetClientSize(esize);
    
    wxSize fsize = GetSize();
    fsize.y = wxMin(fsize.y, dsize.y - (2 * GetCharHeight()));
    fsize.x = wxMin(fsize.x, dsize.x - (1 * GetCharHeight()));
    
    SetSize(fsize);

    if(m_bcompact){
        int sy = GetCharHeight() * m_wpList->GetItemCount();
        sy = wxMax(sy, 250);
        sy = wxMin(sy, 500);
        m_wpList->SetSize(wxSize(GetClientSize().x-40, sy) );
        
        if(m_wpList->GetItemCount())
            Layout();
    }
}


void RouteProp::OnRoutePropRightClick( wxListEvent &event )
{
    wxMenu menu;

    if( ! m_pRoute->m_bIsInLayer ) {
            
#ifdef __OCPN_ANDROID__    
        wxFont *pf = OCPNGetFont(_T("Menu"), 0);
            
        // add stuff
        wxMenuItem *editItem = new wxMenuItem(&menu, ID_RCLK_MENU_EDIT_WP, _("Waypoint Properties") + _T("..."));
        editItem->SetFont(*pf);
        menu.Append(editItem);
        
        wxMenuItem *delItem = new wxMenuItem(&menu, ID_RCLK_MENU_DELETE, _("Remove Selected"));
        delItem->SetFont(*pf);
        menu.Append(delItem);
        
        
#else    
            
        wxMenuItem* editItem = menu.Append( ID_RCLK_MENU_EDIT_WP, _("&Waypoint Properties...") );
        
        wxMenuItem* delItem = menu.Append( ID_RCLK_MENU_DELETE, _("&Remove Selected") );
        
#endif
            
        editItem->Enable( m_wpList->GetSelectedItemCount() == 1 );
        delItem->Enable( m_wpList->GetSelectedItemCount() > 0 && m_wpList->GetItemCount() > 2 );
        
    }

    #ifndef __WXQT__    
    wxMenuItem* copyItem = menu.Append( ID_RCLK_MENU_COPY_TEXT, _("&Copy all as text") );
    #endif

    PopupMenu( &menu,  ::wxGetMousePosition() );
}

void RouteProp::OnRoutepropSplitClick( wxCommandEvent& event )
{
    m_SplitButton->Enable( false );

    if( m_pRoute->m_bIsInLayer ) return;

    if( ( m_nSelected > 1 ) && ( m_nSelected < m_pRoute->GetnPoints() ) ) {
        m_pHead = new Route();
        m_pTail = new Route();
        m_pHead->CloneRoute( m_pRoute, 1, m_nSelected, _("_A") );
        m_pTail->CloneRoute( m_pRoute, m_nSelected, m_pRoute->GetnPoints(), _("_B") );
        pRouteList->Append( m_pHead );
        pConfig->AddNewRoute( m_pHead );

        pRouteList->Append( m_pTail );
        pConfig->AddNewRoute( m_pTail );

        pConfig->DeleteConfigRoute( m_pRoute );

        pSelect->DeleteAllSelectableRoutePoints( m_pRoute );
        pSelect->DeleteAllSelectableRouteSegments( m_pRoute );
        g_pRouteMan->DeleteRoute( m_pRoute );
        pSelect->AddAllSelectableRouteSegments( m_pTail );
        pSelect->AddAllSelectableRoutePoints( m_pTail );
        pSelect->AddAllSelectableRouteSegments( m_pHead );
        pSelect->AddAllSelectableRoutePoints( m_pHead );
        
        SetRouteAndUpdate( m_pTail );
        UpdateProperties();

        if( pRouteManagerDialog && pRouteManagerDialog->IsShown() )
            pRouteManagerDialog->UpdateRouteListCtrl();
    }
}


// slot on pressed button "Print Route" with selection of the route properties to print
void RouteProp::OnRoutepropPrintClick( wxCommandEvent& event )
{
    RoutePrintSelection dlg( GetParent(), m_pRoute );
    dlg.ShowModal();
}

void RouteProp::OnRoutepropExtendClick( wxCommandEvent& event )
{
    m_ExtendButton->Enable( false );

    if( IsThisRouteExtendable() ) {
        int fm = m_pExtendRoute->GetIndexOf( m_pExtendPoint ) + 1;
        int to = m_pExtendRoute->GetnPoints();
        if( fm <= to ) {
            pSelect->DeleteAllSelectableRouteSegments( m_pRoute );
            m_pRoute->CloneRoute( m_pExtendRoute, fm, to, _("_plus") );
            pSelect->AddAllSelectableRouteSegments( m_pRoute );
            SetRouteAndUpdate( m_pRoute );
            UpdateProperties();
        }
    }
}

void RouteProp::OnRoutepropCopyTxtClick( wxCommandEvent& event )
{
    wxString tab("\t", wxConvUTF8);
    wxString eol("\n", wxConvUTF8);
    wxString csvString;

    csvString << this->GetTitle() << eol
            << _("Name") << tab << m_pRoute->m_RouteNameString << eol
            << _("Depart From") << tab << m_pRoute->m_RouteStartString << eol
            << _("Destination") << tab << m_pRoute->m_RouteEndString << eol
            << _("Total distance") << tab << m_TotalDistCtl->GetValue() << eol
            << _("Speed (Kts)") << tab << m_PlanSpeedCtl->GetValue() << eol
            << _("Departure Time") + _T(" ") + _("(m/d/y h:m)") << tab << m_StartTimeCtl->GetValue() << eol
            << _("Time enroute") << tab << m_TimeEnrouteCtl->GetValue() << eol << eol;

    int noCols;
    int noRows;
    noCols = m_wpList->GetColumnCount();
    noRows = m_wpList->GetItemCount();
    wxListItem item;
    item.SetMask( wxLIST_MASK_TEXT );

    for( int i = 0; i < noCols; i++ ) {
        m_wpList->GetColumn( i, item );
        csvString << item.GetText() << tab;
    }
    csvString << eol;

    for( int j = 0; j < noRows; j++ ) {
        item.SetId( j );
        for( int i = 0; i < noCols; i++ ) {
            item.SetColumn( i );

            m_wpList->GetItem( item );

            csvString << item.GetText() << tab;
        }
        csvString << eol;
    }

    if( wxTheClipboard->Open() ) {
        wxTextDataObject* data = new wxTextDataObject;
        data->SetText( csvString );
        wxTheClipboard->SetData( data );
        wxTheClipboard->Close();
    }
}

bool RouteProp::IsThisRouteExtendable()
{
    m_pExtendRoute = NULL;
    m_pExtendPoint = NULL;
    if( m_pRoute->m_bRtIsActive || m_pRoute->m_bIsInLayer ) return false;

    RoutePoint *pLastPoint = m_pRoute->GetLastPoint();
    wxArrayPtrVoid *pEditRouteArray;

    pEditRouteArray = g_pRouteMan->GetRouteArrayContaining( pLastPoint );
    // remove invisible & own routes from choices
    int i;
    for( i = pEditRouteArray->GetCount(); i > 0; i-- ) {
        Route *p = (Route *) pEditRouteArray->Item( i - 1 );
        if( !p->IsVisible() || ( p->m_GUID == m_pRoute->m_GUID ) ) pEditRouteArray->RemoveAt(
            i - 1 );
    }
    if( pEditRouteArray->GetCount() == 1 ) m_pExtendPoint = pLastPoint;
    else
        if( pEditRouteArray->GetCount() == 0 ) {

            int nearby_radius_meters = (int) ( 8. / gFrame->GetPrimaryCanvas()->GetCanvasTrueScale() );
            double rlat = pLastPoint->m_lat;
            double rlon = pLastPoint->m_lon;

            m_pExtendPoint = pWayPointMan->GetOtherNearbyWaypoint( rlat, rlon,
                                                                   nearby_radius_meters, pLastPoint->m_GUID );
            if( m_pExtendPoint ) {
                wxArrayPtrVoid *pCloseWPRouteArray = g_pRouteMan->GetRouteArrayContaining(
                    m_pExtendPoint );
                if( pCloseWPRouteArray ) {
                    pEditRouteArray = pCloseWPRouteArray;

                    // remove invisible & own routes from choices
                    for( i = pEditRouteArray->GetCount(); i > 0; i-- ) {
                        Route *p = (Route *) pEditRouteArray->Item( i - 1 );
                        if( !p->IsVisible() || ( p->m_GUID == m_pRoute->m_GUID ) ) pEditRouteArray->RemoveAt(
                            i - 1 );
                    }
                }
            }
        }

    if( pEditRouteArray->GetCount() == 1 ) {
        Route *p = (Route *) pEditRouteArray->Item( 0 );
        int fm = p->GetIndexOf( m_pExtendPoint ) + 1;
        int to = p->GetnPoints();
        if( fm <= to ) {
            m_pExtendRoute = p;
            delete pEditRouteArray;
            return true;
        }
    }
    delete pEditRouteArray;

    return false;
}


RouteProp::~RouteProp()
{
    delete m_TotalDistCtl;
    delete m_PlanSpeedCtl;
    delete m_TimeEnrouteCtl;

    delete m_RouteNameCtl;
    delete m_RouteStartCtl;
    delete m_RouteDestCtl;

    delete m_StartTimeCtl;

    delete m_wpList;

    instanceFlag = false;
}


/*!
 * Control creation for RouteProp
 */

void RouteProp::CreateControlsCompact()
{
     

    wxBoxSizer* itemBoxSizer1 = new wxBoxSizer( wxVERTICAL );
    SetSizer( itemBoxSizer1 );

    itemDialog1 = new wxScrolledWindow( this, wxID_ANY,
                                      wxDefaultPosition, wxSize(-1, -1), wxVSCROLL);
    itemDialog1->SetScrollRate(0, 1);
    

#ifdef __OCPN__ANDROID__
    //  Set Dialog Font by custom crafted Qt Stylesheet.
    wxFont *qFont = GetOCPNScaledFont(_("Dialog"));
    
    wxString wqs = getFontQtStylesheet(qFont);
    wxCharBuffer sbuf = wqs.ToUTF8();
    QString qsb = QString(sbuf.data());
    
    QString qsbq = getQtStyleSheet();           // basic scrollbars, etc
    
    itemDialog1->GetHandle()->setStyleSheet( qsb + qsbq );      // Concatenated style sheets
    
#endif
    itemBoxSizer1->Add( itemDialog1, 1, wxEXPAND | wxALL, 0 );

    wxBoxSizer* itemBoxSizer2 = new wxBoxSizer( wxVERTICAL );
    itemDialog1->SetSizer( itemBoxSizer2 );


    wxStaticText* itemStaticText4 = new wxStaticText( itemDialog1, wxID_STATIC, _("Name"),
            wxDefaultPosition, wxDefaultSize, 0 );
    itemBoxSizer2->Add( itemStaticText4, 0,
                              wxALIGN_LEFT | wxLEFT | wxRIGHT | wxTOP, 5 );

    m_RouteNameCtl = new wxTextCtrl( itemDialog1, ID_TEXTCTRL, _T(""), wxDefaultPosition,
            wxSize( 400, -1 ), 0 );
    itemBoxSizer2->Add( m_RouteNameCtl, 0,
                        wxALIGN_LEFT | wxLEFT | wxRIGHT | wxBOTTOM , 5 );

    wxStaticText* itemStaticText7 = new wxStaticText( itemDialog1, wxID_STATIC, _("Depart From"),
            wxDefaultPosition, wxDefaultSize, 0 );
    itemBoxSizer2->Add( itemStaticText7, 0,
            wxALIGN_LEFT | wxALIGN_CENTER_VERTICAL | wxLEFT | wxRIGHT, 5 );

    m_RouteStartCtl = new wxTextCtrl( itemDialog1, ID_TEXTCTRL2, _T(""), wxDefaultPosition,
                                      wxSize( -1, -1 ), 0 );
    itemBoxSizer2->Add( m_RouteStartCtl, 0,
                              wxEXPAND | wxALIGN_LEFT | wxALIGN_CENTER_VERTICAL | wxLEFT | wxRIGHT | wxBOTTOM, 5 );
    
    wxStaticText* itemStaticText8 = new wxStaticText( itemDialog1, wxID_STATIC, _("Destination"),
            wxDefaultPosition, wxDefaultSize, 0 );
    itemBoxSizer2->Add( itemStaticText8, 0,
            wxALIGN_LEFT | wxALIGN_CENTER_VERTICAL | wxLEFT | wxRIGHT, 5 );


    m_RouteDestCtl = new wxTextCtrl( itemDialog1, ID_TEXTCTRL1, _T(""), wxDefaultPosition,
            wxSize( -1, -1 ), 0 );
    itemBoxSizer2->Add( m_RouteDestCtl, 0,
                        wxEXPAND | wxALIGN_LEFT | wxALIGN_CENTER_VERTICAL | wxLEFT | wxRIGHT | wxBOTTOM, 5 );

    
    
    wxFlexGridSizer* itemFlexGridSizer6a = new wxFlexGridSizer( 4, 2, 0, 0 );
    itemFlexGridSizer6a->AddGrowableCol(1, 0);
    
    itemBoxSizer2->Add( itemFlexGridSizer6a, 0, wxEXPAND | wxALIGN_LEFT | wxALL, 5 );

    wxStaticText* itemStaticText11 = new wxStaticText( itemDialog1, wxID_STATIC,
            _("Total distance"), wxDefaultPosition, wxDefaultSize, 0 );
    itemFlexGridSizer6a->Add( itemStaticText11, 0,
            wxALIGN_LEFT | wxALIGN_CENTER_VERTICAL | wxLEFT | wxRIGHT | wxTOP,
            5 );

    m_TotalDistCtl = new wxTextCtrl( itemDialog1, ID_TEXTCTRL3, _T(""), wxDefaultPosition,
                                     wxSize( -1, -1 ), wxTE_READONLY );
    itemFlexGridSizer6a->Add( m_TotalDistCtl, 0,
                              wxEXPAND | wxALIGN_LEFT | wxALIGN_CENTER_VERTICAL | wxLEFT | wxRIGHT | wxBOTTOM, 5 );
    
    
    
    
    m_PlanSpeedLabel = new wxStaticText( itemDialog1, wxID_STATIC, _("Plan speed"),
            wxDefaultPosition, wxDefaultSize, 0 );
    itemFlexGridSizer6a->Add( m_PlanSpeedLabel, 0,
            wxALIGN_LEFT | wxALIGN_CENTER_VERTICAL | wxLEFT | wxRIGHT | wxTOP,
            5 );

    m_PlanSpeedCtl = new wxTextCtrl( itemDialog1, ID_PLANSPEEDCTL, _T(""), wxDefaultPosition,
                                     wxSize( 150, -1 ), wxTE_PROCESS_ENTER );
    itemFlexGridSizer6a->Add( m_PlanSpeedCtl, 0,
                              wxALIGN_LEFT | wxALIGN_CENTER_VERTICAL | wxLEFT | wxRIGHT | wxBOTTOM, 5 );
    
    
    
    
    wxStaticText* itemStaticText12a = new wxStaticText( itemDialog1, wxID_STATIC, _("Time enroute"),
            wxDefaultPosition, wxDefaultSize, 0 );
    itemFlexGridSizer6a->Add( itemStaticText12a, 0,
            wxALIGN_LEFT | wxALIGN_CENTER_VERTICAL | wxLEFT | wxRIGHT | wxTOP,
            5 );

    m_TimeEnrouteCtl = new wxTextCtrl( itemDialog1, ID_TEXTCTRL4, _T(""), wxDefaultPosition,
                                       wxSize( -1, -1 ), wxTE_READONLY );
    itemFlexGridSizer6a->Add( m_TimeEnrouteCtl, 0,
                              wxEXPAND | wxALIGN_LEFT | wxALIGN_CENTER_VERTICAL | wxLEFT | wxRIGHT | wxBOTTOM, 5 );
    
    
    m_StartTimeLabel = new wxStaticText( itemDialog1, wxID_STATIC, _("Departure Time"),
            wxDefaultPosition, wxDefaultSize, 0 );
    itemFlexGridSizer6a->Add( m_StartTimeLabel, 0,
            wxALIGN_LEFT | wxALIGN_CENTER_VERTICAL | wxLEFT | wxRIGHT | wxTOP,
            5 );

    m_StartTimeCtl = new wxTextCtrl( itemDialog1, ID_STARTTIMECTL, _T(""), wxDefaultPosition,
            wxSize( -1, -1 ), wxTE_PROCESS_ENTER );
    itemFlexGridSizer6a->Add( m_StartTimeCtl, 0,
            wxEXPAND | wxALIGN_LEFT | wxALIGN_CENTER_VERTICAL | wxLEFT | wxRIGHT | wxBOTTOM, 5 );

    wxString pDispTimeZone[] = { _("UTC"), _("Local @ PC"), _("LMT @ Location") };
    
    wxStaticText* itemStaticText12b = new wxStaticText( itemDialog1, wxID_STATIC, _("Time shown as"),
                                                                                    wxDefaultPosition, wxDefaultSize, 0 );
    itemBoxSizer2->Add( itemStaticText12b, 0, wxEXPAND | wxALL, 5 );
    

    m_prb_tzUTC = new wxRadioButton(itemDialog1, ID_TIMEZONESEL_UTC, _("UTC"), wxDefaultPosition, wxDefaultSize, wxRB_GROUP);
    itemBoxSizer2->Add( m_prb_tzUTC, 0, wxALIGN_LEFT | wxALIGN_CENTER_VERTICAL | wxLEFT | wxRIGHT | wxBOTTOM,5 );
 
    m_prb_tzLocal = new wxRadioButton(itemDialog1, ID_TIMEZONESEL_LOCAL, _("Local @ PC"));
    itemBoxSizer2->Add( m_prb_tzLocal, 0, wxALIGN_LEFT | wxALIGN_CENTER_VERTICAL | wxLEFT | wxRIGHT | wxBOTTOM,5 );

    m_prb_tzLMT = new wxRadioButton(itemDialog1, ID_TIMEZONESEL_LMT, _("LMT @ Location"));
    itemBoxSizer2->Add( m_prb_tzLMT, 0, wxALIGN_LEFT | wxALIGN_CENTER_VERTICAL | wxLEFT | wxRIGHT | wxBOTTOM,5 );

    
    wxFlexGridSizer* itemFlexGridSizer6b = new wxFlexGridSizer( 3, 2, 0, 0 );
    itemBoxSizer2->Add( itemFlexGridSizer6b, 0, wxEXPAND | wxALIGN_LEFT | wxALL, 5 );
    
    m_staticText1 = new wxStaticText( itemDialog1, wxID_ANY, _("Color") + _T(":"), wxDefaultPosition, wxDefaultSize,
            0 );
    itemFlexGridSizer6b->Add( m_staticText1, 0, wxALIGN_CENTER_VERTICAL | wxALL, 5 );

    wxString m_chColorChoices[] = { _("Default color"), _("Black"), _("Dark Red"), _("Dark Green"),
            _("Dark Yellow"), _("Dark Blue"), _("Dark Magenta"), _("Dark Cyan"),
            _("Light Gray"), _("Dark Gray"), _("Red"), _("Green"), _("Yellow"), _("Blue"),
            _("Magenta"), _("Cyan"), _("White") };
    int m_chColorNChoices = sizeof( m_chColorChoices ) / sizeof(wxString);
    m_chColor = new wxChoice( itemDialog1, wxID_ANY, wxDefaultPosition, wxSize(250, -1), m_chColorNChoices,
            m_chColorChoices, 0 );
    m_chColor->SetSelection( 0 );
    itemFlexGridSizer6b->Add( m_chColor, 0,  wxALIGN_CENTER_VERTICAL | wxALL, 5 );

    wxStaticText *staticTextStyle = new wxStaticText( itemDialog1, wxID_ANY, _("Style") + _T(":"), wxDefaultPosition, wxDefaultSize,
            0 );
    itemFlexGridSizer6b->Add( staticTextStyle, 0, wxALIGN_CENTER_VERTICAL | wxALL, 5 );

    wxString m_chStyleChoices[] = { _("Default"), _("Solid"), _("Dot"), _("Long dash"),
            _("Short dash"), _("Dot dash") };
    int m_chStyleNChoices = sizeof( m_chStyleChoices ) / sizeof(wxString);
    m_chStyle = new wxChoice( itemDialog1, wxID_ANY, wxDefaultPosition, wxDefaultSize, m_chStyleNChoices,
            m_chStyleChoices, 0 );
    m_chStyle->SetSelection( 0 );
    itemFlexGridSizer6b->Add( m_chStyle, 0, wxEXPAND | wxALIGN_CENTER_VERTICAL | wxALL, 5 );

#ifdef ocpnUSE_GLES // linestipple is emulated poorly
    staticTextStyle->Hide();
    m_chStyle->Hide();
#endif    
    
    
    m_staticText2 = new wxStaticText( itemDialog1, wxID_ANY, _("Width") + _T(":"), wxDefaultPosition, wxDefaultSize,
            0 );
    itemFlexGridSizer6b->Add( m_staticText2, 0, wxALIGN_CENTER_VERTICAL | wxALL, 5 );

    wxString m_chWidthChoices[] = { _("Default"), _("1 pixel"), _("2 pixels"), _("3 pixels"),
            _("4 pixels"), _("5 pixels"), _("6 pixels"), _("7 pixels"), _("8 pixels"),
            _("9 pixels"), _("10 pixels") };
    int m_chWidthNChoices = sizeof( m_chWidthChoices ) / sizeof(wxString);
    m_chWidth = new wxChoice( itemDialog1, wxID_ANY, wxDefaultPosition, wxSize(150, -1), m_chWidthNChoices,
            m_chWidthChoices, 0 );
    m_chWidth->SetSelection( 0 );
    itemFlexGridSizer6b->Add( m_chWidth, 0, wxEXPAND | wxALIGN_CENTER_VERTICAL | wxALL, 5 );


 
    wxStaticBox* itemStaticBoxSizer14Static = new wxStaticBox( itemDialog1, wxID_ANY, _("Waypoints") );
    m_pListSizer = new wxStaticBoxSizer( itemStaticBoxSizer14Static, wxVERTICAL );
    itemBoxSizer2->Add( m_pListSizer, 1, wxEXPAND | wxALL, 1 );
 
    
    wxScrolledWindow *itemlistWin = new wxScrolledWindow( itemDialog1, wxID_ANY,
                                                          wxDefaultPosition, wxDefaultSize, wxHSCROLL | wxVSCROLL);
    itemlistWin->SetScrollRate(2, 2);
    
    
    
    m_pListSizer->Add( itemlistWin, 0, wxEXPAND | wxALL, 6 );
    
    
    //      Create the list control
    m_wpList = new wxListCtrl( itemlistWin, ID_LISTCTRL, wxDefaultPosition, wxSize( 100, -1 ),
                               wxLC_REPORT | wxLC_HRULES | wxLC_VRULES );
    
    
#ifdef __OCPN__ANDROID__
        m_wpList->GetHandle()->setStyleSheet( getQtStyleSheet());
#endif    
        //  Buttons, etc...

/*        
        wxBoxSizer* itemBoxSizerBottom = new wxBoxSizer( wxVERTICAL );
        itemBoxSizer1->Add( itemBoxSizerBottom, 0, wxALIGN_LEFT | wxALL | wxEXPAND, 5 );
        
        
        int n_col = 3;
        
        wxFlexGridSizer* itemBoxSizerAux = new wxFlexGridSizer( 0, n_col, 0, 0 );
        itemBoxSizerAux->SetFlexibleDirection( wxBOTH );
        itemBoxSizerAux->SetNonFlexibleGrowMode( wxFLEX_GROWMODE_SPECIFIED );
        
        
        
        itemBoxSizerBottom->Add( itemBoxSizerAux, 1, wxALIGN_LEFT | wxALL, 5 );
        
#ifndef __OCPN__ANDROID__
        m_PrintButton = new wxButton( this, wxID_ANY, _("Print"), wxDefaultPosition, wxDefaultSize, 0 );
        itemBoxSizerAux->Add( m_PrintButton, 0, wxALIGN_LEFT | wxALIGN_CENTER_VERTICAL | wxALL, 5 );
        m_PrintButton->Enable( true );
#else
        m_PrintButton = NULL;
#endif
        
        m_SplitButton = new wxButton( this, wxID_ANY, _("Split"), wxDefaultPosition, wxDefaultSize, 0 );
        itemBoxSizerAux->Add( m_SplitButton, 0, wxALIGN_LEFT | wxALIGN_CENTER_VERTICAL | wxALL, 5 );
        m_SplitButton->Enable( false );
        
        m_ExtendButton = new wxButton( this, wxID_ANY, _("Extend"), wxDefaultPosition, wxDefaultSize, 0 );
        itemBoxSizerAux->Add( m_ExtendButton, 0, wxALIGN_LEFT | wxALIGN_CENTER_VERTICAL | wxALL, 5 );
        
        wxBoxSizer* itemBoxSizer16 = new wxBoxSizer( wxHORIZONTAL );
        itemBoxSizerBottom->Add( itemBoxSizer16, 0, wxALIGN_RIGHT | wxALL, 5 );
        
        m_CancelButton = new wxButton( this, wxID_CANCEL, _("Cancel"), wxDefaultPosition, wxDefaultSize, 0 );
        itemBoxSizer16->Add( m_CancelButton, 0, wxALIGN_RIGHT | wxALIGN_BOTTOM | wxALL, 5 );
        
        m_OKButton = new wxButton( this, wxID_OK, _("OK"), wxDefaultPosition, wxDefaultSize, 0 );
        itemBoxSizer16->Add( m_OKButton, 0, wxALIGN_RIGHT | wxALIGN_BOTTOM | wxALL, 5 );
        m_OKButton->SetDefault();
*/        
    
    wxBoxSizer* itemBoxSizerBottom = new wxBoxSizer( wxVERTICAL );
    itemBoxSizer1->Add( itemBoxSizerBottom, 0, wxALIGN_LEFT | wxALL | wxEXPAND, 5 );
    
    wxBoxSizer* itemBoxSizerAux = new wxBoxSizer( wxHORIZONTAL );
    itemBoxSizerBottom->Add( itemBoxSizerAux, 1, wxALIGN_LEFT | wxALL, 3 );

#ifndef __OCPN__ANDROID__
    m_PrintButton = new wxButton( this, ID_ROUTEPROP_PRINT, _("Print Route"),
            wxDefaultPosition, wxDefaultSize, 0 );
     itemBoxSizerAux->Add( m_PrintButton, 0, wxALIGN_LEFT | wxALIGN_CENTER_VERTICAL | wxALL, 3 );
    m_PrintButton->Enable( true );
#else
    m_PrintButton = NULL;
#endif
    
    m_ExtendButton = new wxButton( this, ID_ROUTEPROP_EXTEND, _("Extend Route"),
            wxDefaultPosition, wxDefaultSize, 0 );
    itemBoxSizerAux->Add( m_ExtendButton, 0, wxALIGN_LEFT | wxALIGN_CENTER_VERTICAL | wxALL, 3 );
    m_ExtendButton->Enable( false );

    m_SplitButton = new wxButton( this, ID_ROUTEPROP_SPLIT, _("Split Route"),
            wxDefaultPosition, wxDefaultSize, 0 );
    itemBoxSizerAux->Add( m_SplitButton, 0, wxALIGN_LEFT | wxALIGN_CENTER_VERTICAL | wxALL, 3 );
    m_SplitButton->Enable( false );

    wxBoxSizer* itemBoxSizer16 = new wxBoxSizer( wxHORIZONTAL );
    itemBoxSizerBottom->Add( itemBoxSizer16, 0, wxALIGN_RIGHT | wxALL, 3 );

    m_CancelButton = new wxButton( this, ID_ROUTEPROP_CANCEL, _("Cancel"), wxDefaultPosition,
            wxDefaultSize, 0 );
    itemBoxSizer16->Add( m_CancelButton, 0, wxALIGN_RIGHT | wxALIGN_CENTER_VERTICAL | wxALL, 1 );

    m_OKButton = new wxButton( this, ID_ROUTEPROP_OK, _("OK"), wxDefaultPosition,
            wxDefaultSize, 0 );
    itemBoxSizer16->Add( m_OKButton, 0, wxALIGN_RIGHT | wxALIGN_CENTER_VERTICAL | wxALL, 1);
    m_OKButton->SetDefault();



    //      To correct a bug in MSW commctl32, we need to catch column width drag events, and do a Refresh()
    //      Otherwise, the column heading disappear.....
    //      Does no harm for GTK builds, so no need for conditional
    Connect( wxEVT_COMMAND_LIST_COL_END_DRAG,
            (wxObjectEventFunction) (wxEventFunction) &RouteProp::OnEvtColDragEnd );


    
    
    int char_size = GetCharWidth();

#define C(COL) (cols[COL] = c++)
    int c = 0;

    m_wpList->InsertColumn( C(LEG), _("Leg"), wxLIST_FORMAT_LEFT, 10 );
    m_wpList->InsertColumn( C(TO_WAYPOINT), _("To Waypoint"), wxLIST_FORMAT_LEFT, char_size * 14 );
    m_wpList->InsertColumn( C(DISTANCE), _("Distance"), wxLIST_FORMAT_RIGHT, char_size * 9 );

    if(g_bShowTrue)
        m_wpList->InsertColumn( C(BEARING), _("Bearing"), wxLIST_FORMAT_LEFT, char_size * 10 );
    if(g_bShowMag)
        m_wpList->InsertColumn( C(BEARING_MAGNETIC), _("Bearing (M)"), wxLIST_FORMAT_LEFT, char_size * 10 );

    m_wpList->InsertColumn( C(LATITUDE), _("Latitude"), wxLIST_FORMAT_LEFT, char_size * 11 );
    m_wpList->InsertColumn( C(LONGITUDE), _("Longitude"), wxLIST_FORMAT_LEFT, char_size * 11 );
    m_wpList->InsertColumn( C(ETE_ETD), _("ETE/ETD"), wxLIST_FORMAT_LEFT, char_size * 15 );
    m_wpList->InsertColumn( C(SPEED), _("Speed"), wxLIST_FORMAT_CENTER, char_size * 9 );
    m_wpList->InsertColumn( C(NEXT_TIDE), _("Next tide event"), wxLIST_FORMAT_LEFT, char_size * 11 );
    m_wpList->InsertColumn( C(DESCRIPTION), _("Description"), wxLIST_FORMAT_LEFT, char_size * 11 );

    if(g_bShowTrue)
        m_wpList->InsertColumn( C(COURSE), _("Course"), wxLIST_FORMAT_LEFT, char_size * 10 );
    if(g_bShowMag)
        m_wpList->InsertColumn( C(COURSE_MAGNETIC), _("Course (M)"), wxLIST_FORMAT_LEFT, char_size * 10 );
   
    //Set the maximum size of the entire  dialog
    int width, height;
    ::wxDisplaySize( &width, &height );
    SetSizeHints( -1, -1, width-100, height-100 );
    
    Connect( wxEVT_COMMAND_LIST_ITEM_RIGHT_CLICK,
            wxListEventHandler(RouteProp::OnRoutePropRightClick), NULL, this );
    Connect( wxEVT_COMMAND_MENU_SELECTED,
            wxCommandEventHandler(RouteProp::OnRoutePropMenuSelected), NULL, this );

    //  Fetch any config file values
    m_planspeed = g_PlanSpeed;

    if( g_StartTimeTZ == 0 )
        m_prb_tzUTC->SetValue( true);
    else if( g_StartTimeTZ == 1 )
        m_prb_tzLocal->SetValue( true);
    else if( g_StartTimeTZ == 2 )
        m_prb_tzLMT->SetValue( true);
    

    SetColorScheme( (ColorScheme) 0 );

    
}

void RouteProp::CreateControls()
{
    
    
    wxBoxSizer* itemBoxSizer1 = new wxBoxSizer( wxVERTICAL );
    SetSizer( itemBoxSizer1 );
    
    itemDialog1 = new wxScrolledWindow( this, wxID_ANY,
                                        wxDefaultPosition, wxDefaultSize, wxHSCROLL | wxVSCROLL);
    itemDialog1->SetScrollRate(2, 2);
    
    #ifdef __OCPN__ANDROID__
    //itemDialog1->GetHandle()->setStyleSheet( getQtStyleSheet());
    
    //  Set Dialog Font by custom crafted Qt Stylesheet.
    wxFont *qFont = GetOCPNScaledFont(_("Dialog"));
    
    wxString wqs = getFontQtStylesheet(qFont);
    wxCharBuffer sbuf = wqs.ToUTF8();
    QString qsb = QString(sbuf.data());
    
    QString qsbq = getQtStyleSheet();           // basic scrollbars, etc
    
    itemDialog1->GetHandle()->setStyleSheet( qsb + qsbq );      // Concatenated style sheets
    
    #endif
    itemBoxSizer1->Add( itemDialog1, 2, wxEXPAND | wxALL, 0 );
    
    wxBoxSizer* itemBoxSizer2 = new wxBoxSizer( wxVERTICAL );
    itemDialog1->SetSizer( itemBoxSizer2 );
    
    wxStaticBox* itemStaticBoxSizer3Static = new wxStaticBox( itemDialog1, wxID_ANY,
                                                              _("Properties") );
    wxStaticBoxSizer* itemStaticBoxSizer3 = new wxStaticBoxSizer( itemStaticBoxSizer3Static,
                                                                  wxVERTICAL );
    itemBoxSizer2->Add( itemStaticBoxSizer3, 0, wxEXPAND | wxALL, 5 );
    
    wxStaticText* itemStaticText4 = new wxStaticText( itemDialog1, wxID_STATIC, _("Name"),
                                                      wxDefaultPosition, wxDefaultSize, 0 );
    itemStaticBoxSizer3->Add( itemStaticText4, 0,
                              wxALIGN_LEFT | wxLEFT | wxRIGHT | wxTOP, 5 );
    
    m_RouteNameCtl = new wxTextCtrl( itemDialog1, ID_TEXTCTRL, _T(""), wxDefaultPosition,
                                     wxSize( 710, -1 ), 0 );
    itemStaticBoxSizer3->Add( m_RouteNameCtl, 0,
                              wxALIGN_LEFT | wxLEFT | wxRIGHT | wxBOTTOM | wxEXPAND, 5 );
    
    wxFlexGridSizer* itemFlexGridSizer6 = new wxFlexGridSizer( 2, 2, 0, 0 );
    itemStaticBoxSizer3->Add( itemFlexGridSizer6, 1, wxALIGN_LEFT | wxALL, 5 );
    
    wxStaticText* itemStaticText7 = new wxStaticText( itemDialog1, wxID_STATIC, _("Depart From"),
                                                      wxDefaultPosition, wxDefaultSize, 0 );
    itemFlexGridSizer6->Add( itemStaticText7, 0,
                             wxALIGN_LEFT | wxALIGN_CENTER_VERTICAL | wxLEFT | wxRIGHT, 5 );
    
    wxStaticText* itemStaticText8 = new wxStaticText( itemDialog1, wxID_STATIC, _("Destination"),
                                                      wxDefaultPosition, wxDefaultSize, 0 );
    itemFlexGridSizer6->Add( itemStaticText8, 0,
                             wxALIGN_LEFT | wxALIGN_CENTER_VERTICAL | wxLEFT | wxRIGHT, 5 );
    
    m_RouteStartCtl = new wxTextCtrl( itemDialog1, ID_TEXTCTRL2, _T(""), wxDefaultPosition,
                                      wxSize( 300, -1 ), 0 );
    itemFlexGridSizer6->Add( m_RouteStartCtl, 0,
                             wxALIGN_LEFT | wxALIGN_CENTER_VERTICAL | wxLEFT | wxRIGHT | wxBOTTOM, 5 );
    
    m_RouteDestCtl = new wxTextCtrl( itemDialog1, ID_TEXTCTRL1, _T(""), wxDefaultPosition,
                                     wxSize( 300, -1 ), 0 );
    itemFlexGridSizer6->Add( m_RouteDestCtl, 0,
                             wxALIGN_CENTER_HORIZONTAL | wxALIGN_CENTER_VERTICAL | wxLEFT | wxRIGHT | wxBOTTOM, 5 );
    
    wxFlexGridSizer* itemFlexGridSizer6a = new wxFlexGridSizer( 2, 4, 0, 0 );
    itemStaticBoxSizer3->Add( itemFlexGridSizer6a, 1, wxALIGN_LEFT | wxALL, 5 );
    
    wxStaticText* itemStaticText11 = new wxStaticText( itemDialog1, wxID_STATIC,
                                                       _("Total distance"), wxDefaultPosition, wxDefaultSize, 0 );
    itemFlexGridSizer6a->Add( itemStaticText11, 0,
                              wxALIGN_LEFT | wxALIGN_CENTER_VERTICAL | wxLEFT | wxRIGHT | wxTOP,
                              5 );
    
    m_PlanSpeedLabel = new wxStaticText( itemDialog1, wxID_STATIC, _("Plan speed"),
                                         wxDefaultPosition, wxDefaultSize, 0 );
    itemFlexGridSizer6a->Add( m_PlanSpeedLabel, 0,
                              wxALIGN_LEFT | wxALIGN_CENTER_VERTICAL | wxLEFT | wxRIGHT | wxTOP,
                              5 );
    
    wxStaticText* itemStaticText12a = new wxStaticText( itemDialog1, wxID_STATIC, _("Time enroute"),
                                                        wxDefaultPosition, wxDefaultSize, 0 );
    itemFlexGridSizer6a->Add( itemStaticText12a, 0,
                              wxALIGN_LEFT | wxALIGN_CENTER_VERTICAL | wxLEFT | wxRIGHT | wxTOP,
                              5 );
    
    m_StartTimeLabel = new wxStaticText( itemDialog1, wxID_STATIC, _("Departure Time") + _T(" ") + _("(m/d/y h:m)"),
                                         wxDefaultPosition, wxDefaultSize, 0 );
    itemFlexGridSizer6a->Add( m_StartTimeLabel, 0,
                              wxALIGN_LEFT | wxALIGN_CENTER_VERTICAL | wxLEFT | wxRIGHT | wxTOP,
                              5 );
    
    m_TotalDistCtl = new wxTextCtrl( itemDialog1, ID_TEXTCTRL3, _T(""), wxDefaultPosition,
                                     wxDefaultSize, wxTE_READONLY );
    itemFlexGridSizer6a->Add( m_TotalDistCtl, 0,
                              wxALIGN_LEFT | wxALIGN_CENTER_VERTICAL | wxLEFT | wxRIGHT | wxBOTTOM, 5 );
    
    m_PlanSpeedCtl = new wxTextCtrl( itemDialog1, ID_PLANSPEEDCTL, _T(""), wxDefaultPosition,
                                     wxSize( 100, -1 ), wxTE_PROCESS_ENTER );
    itemFlexGridSizer6a->Add( m_PlanSpeedCtl, 0,
                              wxALIGN_LEFT | wxALIGN_CENTER_VERTICAL | wxLEFT | wxRIGHT | wxBOTTOM, 5 );
    
    m_TimeEnrouteCtl = new wxTextCtrl( itemDialog1, ID_TEXTCTRL4, _T(""), wxDefaultPosition,
                                       wxSize( 200, -1 ), wxTE_READONLY );
    itemFlexGridSizer6a->Add( m_TimeEnrouteCtl, 0,
                              wxALIGN_LEFT | wxALIGN_CENTER_VERTICAL | wxLEFT | wxRIGHT | wxBOTTOM, 5 );
    
    m_StartTimeCtl = new wxTextCtrl( itemDialog1, ID_STARTTIMECTL, _T(""), wxDefaultPosition,
                                     wxSize( 150, -1 ), wxTE_PROCESS_ENTER );
    itemFlexGridSizer6a->Add( m_StartTimeCtl, 0,
                              wxALIGN_LEFT | wxALIGN_CENTER_VERTICAL | wxLEFT | wxRIGHT | wxBOTTOM, 5 );
    
    wxString pDispTimeZone[] = { _("UTC"), _("Local @ PC"), _("LMT @ Location") };
    wxBoxSizer* bSizer2;
    bSizer2 = new wxBoxSizer( wxHORIZONTAL );
    
    wxStaticBox* itemStaticBoxTZ = new wxStaticBox( itemDialog1, wxID_ANY,  _("Time shown as") );
    wxStaticBoxSizer* itemStaticBoxSizerTZ = new wxStaticBoxSizer( itemStaticBoxTZ, wxHORIZONTAL );
    bSizer2->Add( itemStaticBoxSizerTZ, 0, wxEXPAND | wxALL, 5 );
    
    
    m_prb_tzUTC = new wxRadioButton(itemDialog1, ID_TIMEZONESEL_UTC, _("UTC"), wxDefaultPosition, wxDefaultSize, wxRB_GROUP);
    itemStaticBoxSizerTZ->Add( m_prb_tzUTC, 0, wxALIGN_LEFT | wxALIGN_CENTER_VERTICAL | wxLEFT | wxRIGHT | wxBOTTOM,5 );
    
    m_prb_tzLocal = new wxRadioButton(itemDialog1, ID_TIMEZONESEL_LOCAL, _("Local @ PC"));
    itemStaticBoxSizerTZ->Add( m_prb_tzLocal, 0, wxALIGN_LEFT | wxALIGN_CENTER_VERTICAL | wxLEFT | wxRIGHT | wxBOTTOM,5 );
    
    m_prb_tzLMT = new wxRadioButton(itemDialog1, ID_TIMEZONESEL_LMT, _("LMT @ Location"));
    itemStaticBoxSizerTZ->Add( m_prb_tzLMT, 0, wxALIGN_LEFT | wxALIGN_CENTER_VERTICAL | wxLEFT | wxRIGHT | wxBOTTOM,5 );
    
    m_staticText1 = new wxStaticText( itemDialog1, wxID_ANY, _("Color") + _T(":"), wxDefaultPosition, wxDefaultSize,
                                      0 );
    //m_staticText1->Wrap( -1 );
    bSizer2->Add( m_staticText1, 0, wxALIGN_CENTER_VERTICAL | wxALL, 5 );
    
    wxString m_chColorChoices[] = { _("Default color"), _("Black"), _("Dark Red"), _("Dark Green"),
    _("Dark Yellow"), _("Dark Blue"), _("Dark Magenta"), _("Dark Cyan"),
      _("Light Gray"), _("Dark Gray"), _("Red"), _("Green"), _("Yellow"), _("Blue"),
      _("Magenta"), _("Cyan"), _("White") };
      int m_chColorNChoices = sizeof( m_chColorChoices ) / sizeof(wxString);
      m_chColor = new wxChoice( itemDialog1, wxID_ANY, wxDefaultPosition, wxDefaultSize, m_chColorNChoices,
                                m_chColorChoices, 0 );
      m_chColor->SetSelection( 0 );
      bSizer2->Add( m_chColor, 0, wxALIGN_CENTER_VERTICAL | wxALL, 5 );
      
      wxStaticText *staticTextStyle = new wxStaticText( itemDialog1, wxID_ANY, _("Style") + _T(":"), wxDefaultPosition, wxDefaultSize,
        0 );
      bSizer2->Add( staticTextStyle, 0, wxALIGN_CENTER_VERTICAL | wxALL, 5 );
      
      wxString m_chStyleChoices[] = { _("Default"), _("Solid"), _("Dot"), _("Long dash"),
      _("Short dash"), _("Dot dash") };
      int m_chStyleNChoices = sizeof( m_chStyleChoices ) / sizeof(wxString);
      m_chStyle = new wxChoice( itemDialog1, wxID_ANY, wxDefaultPosition, wxDefaultSize, m_chStyleNChoices,
                                m_chStyleChoices, 0 );
      m_chStyle->SetSelection( 0 );
      bSizer2->Add( m_chStyle, 0, wxALIGN_CENTER_VERTICAL | wxALL, 5 );
      
      #ifdef ocpnUSE_GLES // linestipple is emulated poorly
      staticTextStyle->Hide();
      m_chStyle->Hide();
      #endif    
      
      
      m_staticText2 = new wxStaticText( itemDialog1, wxID_ANY, _("Width") + _T(":"), wxDefaultPosition, wxDefaultSize,
        0 );
      //m_staticText2->Wrap( -1 );
      bSizer2->Add( m_staticText2, 0, wxALIGN_CENTER_VERTICAL | wxALL, 5 );
      
      wxString m_chWidthChoices[] = { _("Default"), _("1 pixel"), _("2 pixels"), _("3 pixels"),
      _("4 pixels"), _("5 pixels"), _("6 pixels"), _("7 pixels"), _("8 pixels"),
        _("9 pixels"), _("10 pixels") };
        int m_chWidthNChoices = sizeof( m_chWidthChoices ) / sizeof(wxString);
        m_chWidth = new wxChoice( itemDialog1, wxID_ANY, wxDefaultPosition, wxDefaultSize, m_chWidthNChoices,
                                  m_chWidthChoices, 0 );
        m_chWidth->SetSelection( 0 );
        bSizer2->Add( m_chWidth, 0, wxALIGN_CENTER_VERTICAL | wxALL, 5 );
        
        itemStaticBoxSizer3->Add( bSizer2, 1, wxEXPAND, 0 );
        
        wxStaticBox* itemStaticBoxSizer14Static = new wxStaticBox( this, wxID_ANY, _("Waypoints") );
        m_pListSizer = new wxStaticBoxSizer( itemStaticBoxSizer14Static, wxVERTICAL );
        itemBoxSizer1->Add( m_pListSizer, 2, wxEXPAND | wxALL, 1 );
        
        //      Create the list control
        m_wpList = new wxListCtrl( this, ID_LISTCTRL, wxDefaultPosition, wxSize( -1, -1 ),
                                   wxLC_REPORT | wxLC_HRULES | wxLC_VRULES | wxLC_EDIT_LABELS );
        
        m_wpList->SetMinSize(wxSize(-1, 100) );
        m_pListSizer->Add( m_wpList, 1, wxEXPAND | wxALL, 6 );
        
        
        #ifdef __OCPN__ANDROID__
        m_wpList->GetHandle()->setStyleSheet( getQtStyleSheet());
        #endif    
        
        
        wxBoxSizer* itemBoxSizerBottom = new wxBoxSizer( wxHORIZONTAL );
        itemBoxSizer1->Add( itemBoxSizerBottom, 0, wxALL | wxEXPAND, 5 );
        
        wxBoxSizer* itemBoxSizerAux = new wxBoxSizer( wxHORIZONTAL );
        itemBoxSizerBottom->Add( itemBoxSizerAux, 1, wxALIGN_LEFT | wxALL, 3 );
        
        m_PrintButton = new wxButton( this, ID_ROUTEPROP_PRINT, _("Print Route"),
          wxDefaultPosition, wxDefaultSize, 0 );
        itemBoxSizerAux->Add( m_PrintButton, 0, wxALIGN_LEFT | wxALIGN_CENTER_VERTICAL | wxALL, 3 );
        m_PrintButton->Enable( true );
        
        m_ExtendButton = new wxButton( this, ID_ROUTEPROP_EXTEND, _("Extend Route"),
        wxDefaultPosition, wxDefaultSize, 0 );
      itemBoxSizerAux->Add( m_ExtendButton, 0, wxALIGN_LEFT | wxALIGN_CENTER_VERTICAL | wxALL, 3 );
      m_ExtendButton->Enable( false );
      
      m_SplitButton = new wxButton( this, ID_ROUTEPROP_SPLIT, _("Split Route"),
      wxDefaultPosition, wxDefaultSize, 0 );
      itemBoxSizerAux->Add( m_SplitButton, 0, wxALIGN_LEFT | wxALIGN_CENTER_VERTICAL | wxALL, 3 );
      m_SplitButton->Enable( false );
      
      wxBoxSizer* itemBoxSizer16 = new wxBoxSizer( wxHORIZONTAL );
      itemBoxSizerBottom->Add( itemBoxSizer16, 0, wxALL, 3 );
      
      m_CancelButton = new wxButton( this, ID_ROUTEPROP_CANCEL, _("Cancel"), wxDefaultPosition,
      wxDefaultSize, 0 );
      itemBoxSizer16->Add( m_CancelButton, 0, wxALIGN_CENTER_VERTICAL | wxALL, 1 );
      
      m_OKButton = new wxButton( this, ID_ROUTEPROP_OK, _("OK"), wxDefaultPosition,
      wxDefaultSize, 0 );
      itemBoxSizer16->Add( m_OKButton, 0, wxALIGN_CENTER_VERTICAL | wxALL, 1);
      m_OKButton->SetDefault();
      
      //      To correct a bug in MSW commctl32, we need to catch column width drag events, and do a Refresh()
      //      Otherwise, the column heading disappear.....
      //      Does no harm for GTK builds, so no need for conditional
      Connect( wxEVT_COMMAND_LIST_COL_END_DRAG,
               (wxObjectEventFunction) (wxEventFunction) &RouteProp::OnEvtColDragEnd );
      
      
      
      
      int char_size = GetCharWidth();

#define C(COL) (cols[COL] = c++)
      int c = 0;
      m_wpList->InsertColumn( C(LEG), _("Leg"), wxLIST_FORMAT_LEFT, char_size * 6 );
      m_wpList->InsertColumn( C(TO_WAYPOINT), _("To Waypoint"), wxLIST_FORMAT_LEFT, char_size * 14 );
      m_wpList->InsertColumn( C(DISTANCE), _("Distance"), wxLIST_FORMAT_RIGHT, char_size * 9 );

      if(g_bShowTrue)
          m_wpList->InsertColumn( C(BEARING), _("Bearing"), wxLIST_FORMAT_LEFT, char_size * 10 );
      if(g_bShowMag)
          m_wpList->InsertColumn( C(BEARING_MAGNETIC), _("Bearing (M)"), wxLIST_FORMAT_LEFT, char_size * 10 );
      
      m_wpList->InsertColumn( C(LATITUDE), _("Latitude"), wxLIST_FORMAT_LEFT, char_size * 11 );
      m_wpList->InsertColumn( C(LONGITUDE), _("Longitude"), wxLIST_FORMAT_LEFT, char_size * 11 );
      m_wpList->InsertColumn( C(ETE_ETD), _("ETE/ETD"), wxLIST_FORMAT_LEFT, char_size * 15 );
      m_wpList->InsertColumn( C(SPEED), _("Speed"), wxLIST_FORMAT_CENTER, char_size * 9 );
      m_wpList->InsertColumn( C(NEXT_TIDE), _("Next tide event"), wxLIST_FORMAT_LEFT, char_size * 11 );
      m_wpList->InsertColumn( C(DESCRIPTION), _("Description"), wxLIST_FORMAT_LEFT, char_size * 11 );

      if(g_bShowTrue)
          m_wpList->InsertColumn( C(COURSE), _("Course"), wxLIST_FORMAT_LEFT, char_size * 10 );
      if(g_bShowMag)
          m_wpList->InsertColumn( C(COURSE_MAGNETIC), _("Course (M)"), wxLIST_FORMAT_LEFT, char_size * 10 );
      
      //Set the maximum size of the entire  dialog
      int width, height;
      ::wxDisplaySize( &width, &height );
      SetSizeHints( -1, -1, width-100, height-100 );
      
      Connect( wxEVT_COMMAND_LIST_ITEM_RIGHT_CLICK,
               wxListEventHandler(RouteProp::OnRoutePropRightClick), NULL, this );
      Connect( wxEVT_COMMAND_MENU_SELECTED,
               wxCommandEventHandler(RouteProp::OnRoutePropMenuSelected), NULL, this );
      
      //  Fetch any config file values
      m_planspeed = g_PlanSpeed;
      
      if( g_StartTimeTZ == 0 )
          m_prb_tzUTC->SetValue( true);
      else if( g_StartTimeTZ == 1 )
          m_prb_tzLocal->SetValue( true);
      else if( g_StartTimeTZ == 2 )
          m_prb_tzLMT->SetValue( true);
      
      
      SetColorScheme( (ColorScheme) 0 );
      
      
      }
      
int RouteProp::GetTZSelection(void)
{
    if(m_prb_tzUTC && m_prb_tzUTC->GetValue())
        return 0;

    else if(m_prb_tzLocal && m_prb_tzLocal->GetValue())
        return 1;
    
    else if(m_prb_tzLMT && m_prb_tzLMT->GetValue())
        return 2;
    
    else
        return 0;
}
void RouteProp::OnRoutepropListClick( wxListEvent& event )
{
    long itemno = 0;
    m_nSelected = 0;

    //      We use different methods to determine the selected point,
    //      depending on whether this is a Route or a Track.
    int selected_no;
    const wxListItem &i = event.GetItem();
    i.GetText().ToLong( &itemno );
    selected_no = itemno;

    m_pRoute->ClearHighlights();

    wxRoutePointListNode *node = m_pRoute->pRoutePointList->GetFirst();
    while( node && itemno-- ) {
        node = node->GetNext();
    }
    if( node ) {
        RoutePoint *prp = node->GetData();
        if( prp ) {
            prp->m_bPtIsSelected = true;                // highlight the routepoint

            if( !m_pRoute->m_bIsInLayer && !m_pRoute->m_bRtIsActive ) {
                m_nSelected = selected_no + 1;
                m_SplitButton->Enable( true );
            }

            gFrame->JumpToPosition( gFrame->GetPrimaryCanvas(), prp->m_lat, prp->m_lon, gFrame->GetPrimaryCanvas()->GetVPScale() );

#ifdef __WXMSW__            
            if (m_wpList)
                m_wpList->SetFocus();
#endif            
        }
    }
}


void RouteProp::OnRoutePropMenuSelected( wxCommandEvent& event )
{
    switch( event.GetId() ) {
        case ID_RCLK_MENU_COPY_TEXT: {
            OnRoutepropCopyTxtClick( event );
            break;
        }
        case ID_RCLK_MENU_DELETE: {
            int dlg_return = OCPNMessageBox( this, _("Are you sure you want to remove this waypoint?"),
                    _("OpenCPN Remove Waypoint"), (long) wxYES_NO | wxCANCEL | wxYES_DEFAULT );

            if( dlg_return == wxID_YES ) {
                long item = -1;
                item = m_wpList->GetNextItem( item, wxLIST_NEXT_ALL, wxLIST_STATE_SELECTED );

                if( item == -1 ) break;

                RoutePoint *wp;
                wp = (RoutePoint *) m_wpList->GetItemData( item );

                g_pRouteMan->RemovePointFromRoute( wp, m_pRoute, NULL );
            }
            break;
        }
        case ID_RCLK_MENU_EDIT_WP: {
            long item = -1;

            item = m_wpList->GetNextItem( item, wxLIST_NEXT_ALL, wxLIST_STATE_SELECTED );

            if( item == -1 ) break;

            RoutePoint *wp = (RoutePoint *) m_wpList->GetItemData( item );
            if( !wp ) break;

            RouteManagerDialog::WptShowPropertiesDialog( wp, this );
            break;
        }
    }
}

void RouteProp::SetColorScheme( ColorScheme cs )
{
    DimeControl( this );
}

/*
 * Should we show tooltips?
 */

bool RouteProp::ShowToolTips()
{
    return TRUE;
}

void RouteProp::SetDialogTitle(const wxString & title)
{
    SetTitle(title);
}

void RouteProp::SetRouteAndUpdate( Route *pR, bool only_points )
{
    if( NULL == pR )
        return;

    //  Fetch any config file values
    if ( !only_points )
    {
        //      long LMT_Offset = 0;                    // offset in seconds from UTC for given location (-1 hr / 15 deg W)
        m_tz_selection = 1;

        if( pR == m_pRoute ) {
            gStart_LMT_Offset = 0;
            if( pR->m_PlannedDeparture.IsValid() )
                m_starttime = pR->m_PlannedDeparture;
            else
                m_starttime = g_StartTime;

        } else {
            g_StartTime = wxInvalidDateTime;
            g_StartTimeTZ = 1;
            if( pR->m_PlannedDeparture.IsValid() )
                m_starttime = pR->m_PlannedDeparture;
            else
                m_starttime = g_StartTime;
            if( pR->m_TimeDisplayFormat == RTE_TIME_DISP_UTC)
                m_tz_selection = 0;
            else if( pR->m_TimeDisplayFormat == RTE_TIME_DISP_LOCAL )
                m_tz_selection = 2;
            else
                m_tz_selection = g_StartTimeTZ;
            gStart_LMT_Offset = 0;
            m_pEnroutePoint = NULL;
            m_bStartNow = false;
            m_planspeed = pR->m_PlannedSpeed;
        }

        m_pRoute = pR;
        
        if( g_StartTimeTZ == 0 )
            m_prb_tzUTC->SetValue( true);
        else if( g_StartTimeTZ == 1 )
            m_prb_tzLocal->SetValue( true);
        else if( g_StartTimeTZ == 2 )
            m_prb_tzLMT->SetValue( true);
        

        if( m_pRoute ) {
            //    Calculate  LMT offset from the first point in the route
            if( m_pEnroutePoint && m_bStartNow ) gStart_LMT_Offset = long(
                    ( m_pEnroutePoint->m_lon ) * 3600. / 15. );
            else
                gStart_LMT_Offset = long(
                        ( m_pRoute->pRoutePointList->GetFirst()->GetData()->m_lon ) * 3600. / 15. );
        }

        // Reorganize dialog for route or track display
        if( m_pRoute ) {
            m_PlanSpeedLabel->SetLabel( _("Plan speed") );
            m_PlanSpeedCtl->SetEditable( true );
            m_ExtendButton->SetLabel( _("Extend Route") );
            m_SplitButton->SetLabel( _("Split Route") );

            //    Fill in some top pane properties from the Route member elements
            m_RouteNameCtl->SetValue( m_pRoute->m_RouteNameString );
            m_RouteStartCtl->SetValue( m_pRoute->m_RouteStartString );
            m_RouteDestCtl->SetValue( m_pRoute->m_RouteEndString );

            m_RouteNameCtl->SetFocus();
        } else {
            m_RouteNameCtl->Clear();
            m_RouteStartCtl->Clear();
            m_RouteDestCtl->Clear();
            m_PlanSpeedCtl->Clear();
            m_StartTimeCtl->Clear();
        }
    }
    //      if(m_pRoute)
    //            m_pRoute->UpdateSegmentDistances(m_planspeed);           // to interpret ETD properties

    m_wpList->DeleteAllItems();

#if 0
    // Select the proper list control, and add it to List sizer
    m_pListSizer->Clear();

    if( m_pRoute ) {
        m_wpList->Show();
        m_pListSizer->Add( m_wpList, 2, wxEXPAND | wxALL, 5 );
    }
//    GetSizer()->Fit( this );
    GetSizer()->Layout();
#endif


    InitializeList();

    UpdateProperties();

    if( m_pRoute )
        m_wpList->Show();

//    GetSizer()->Fit( this );
//    GetSizer()->Layout();

    RecalculateSize();
    Refresh( false );

}

void RouteProp::InitializeList()
{
    if( NULL == m_pRoute ) return;

    m_pRoute->UpdateSegmentDistances( m_planspeed );           // to fix ETD properties
    //  Iterate on Route Points, inserting blank fields starting with index 0
    wxRoutePointListNode *pnode = m_pRoute->pRoutePointList->GetFirst();
    int in = 0;
    while( pnode ) {
        m_wpList->InsertItem( in, _T(""), -1 );
        m_wpList->SetItemPtrData( in, (wxUIntPtr)pnode->GetData() );
        in++;
        if( pnode->GetData()->m_seg_etd.IsValid() ) {
            m_wpList->InsertItem( in, _T(""), -1 );
            in++;
        }
        pnode = pnode->GetNext();
    }

    //      Update the plan speed and route start time controls
    wxString s;
    s.Printf( _T("%5.2f"), m_planspeed );
    m_PlanSpeedCtl->SetValue( s );

    if( m_starttime.IsValid() ) {
        wxString s = ts2s( m_starttime, m_tz_selection, (int) gStart_LMT_Offset, INPUT_FORMAT );
        m_StartTimeCtl->SetValue( s );
    } else
        m_StartTimeCtl->Clear();
}

bool RouteProp::UpdateProperties()
{
    if( NULL == m_pRoute ) return false;

    ::wxBeginBusyCursor();

    m_TotalDistCtl->SetValue( _T("") );
    m_TimeEnrouteCtl->SetValue( _T("") );
    int tz_selection = GetTZSelection();
    long LMT_Offset = 0;         // offset in seconds from UTC for given location (-1 hr / 15 deg W)

    m_SplitButton->Enable( false );
    m_ExtendButton->Enable( false );

#if 0
    if( m_pRoute->m_bIsTrack ) {
        m_pRoute->UpdateSegmentDistances();           // get segment and total distance
        // but ignore leg speed calcs

        // Calculate AVG speed if we are showing a track and total time
        RoutePoint *last_point = m_pRoute->GetLastPoint();
        RoutePoint *first_point = m_pRoute->GetPoint( 1 );
        double total_seconds = 0.;

        if( last_point->GetCreateTime().IsValid() && first_point->GetCreateTime().IsValid() ) {
            total_seconds =
                    last_point->GetCreateTime().Subtract( first_point->GetCreateTime() ).GetSeconds().ToDouble();
            if( total_seconds != 0. ) {
                m_avgspeed = m_pRoute->m_route_length / total_seconds * 3600;
            } else {
                m_avgspeed = 0;
            }
            wxString s;
            s.Printf( _T("%5.2f"), toUsrSpeed( m_avgspeed ) );
            m_PlanSpeedCtl->SetValue( s );
        } else {
            wxString s( _T("--") );
            m_PlanSpeedCtl->SetValue( s );
        }

        //  Total length
        wxString slen;
        slen.Printf( wxT("%5.2f ") + getUsrDistanceUnit(), toUsrDistance( m_pRoute->m_route_length ) );

        m_TotalDistCtl->SetValue( slen );

        //  Time
        wxString time_form;
        wxTimeSpan time( 0, 0, (int) total_seconds, 0 );
        if(m_bcompact){
            if( total_seconds > 3600. * 24. )
                time_form = time.Format( _(" %D D  %H H  %M M") );
            else
                if( total_seconds > 0. )
                    time_form = time.Format( _(" %H H  %M M") );
                else
                    time_form = _T("--");
        }
        else{
            if( total_seconds > 3600. * 24. )
                time_form = time.Format( _(" %D Days  %H Hours  %M Minutes") );
            else
                if( total_seconds > 0. )
                    time_form = time.Format( _(" %H Hours  %M Minutes") );
                else
                    time_form = _T("--");
        }
        
        m_TimeEnrouteCtl->SetValue( time_form );

    } else        // Route
#endif        
    {
        double brg;
        double join_distance = 0.;
        RoutePoint *first_point = m_pRoute->GetPoint( 1 );
        if( first_point )
            DistanceBearingMercator( first_point->m_lat, first_point->m_lon, gLat, gLon, &brg, &join_distance );

        //    Update the "tides event" column header
        wxListItem column_info;
        if( m_wpList->GetColumn( cols[NEXT_TIDE], column_info ) ) {
            wxString c = _("Next tide event");
            if( gpIDX && m_starttime.IsValid() ) {
                c = _T("@~~");
                c.Append( wxString( gpIDX->IDX_station_name, wxConvUTF8 ) );
                int i = c.Find( ',' );
                if( i != wxNOT_FOUND ) c.Remove( i );

            }
            column_info.SetText( c );
            m_wpList->SetColumn( cols[NEXT_TIDE], column_info );
        }

        //Update the "ETE/Timestamp" column header

        if( m_wpList->GetColumn( cols[ETE_ETD], column_info ) ) {
            if( m_starttime.IsValid() ) column_info.SetText( _("ETA") );
            else
                column_info.SetText( _("ETE") );
            m_wpList->SetColumn( cols[ETE_ETD], column_info );
        }

        m_pRoute->UpdateSegmentDistances( m_planspeed );           // get segment and total distance
        double leg_speed = m_planspeed;
        wxTimeSpan stopover_time( 0 ); // time spent waiting for ETD
        wxTimeSpan joining_time( 0 );   // time spent before reaching first waypoint

        double total_seconds = 0.;

        if( m_pRoute ) {
            total_seconds = m_pRoute->m_route_time;
            if( m_bStartNow ) {
                if( m_pEnroutePoint ) gStart_LMT_Offset = long(
                        ( m_pEnroutePoint->m_lon ) * 3600. / 15. );
                else
                    gStart_LMT_Offset = long(
                            ( m_pRoute->pRoutePointList->GetFirst()->GetData()->m_lon ) * 3600.
                                    / 15. );
            }
        }

        if( m_bStartNow ) {
            joining_time = wxTimeSpan::Seconds((long) wxRound( ( join_distance * 3600. ) / m_planspeed ) );
            double join_seconds = joining_time.GetSeconds().ToDouble();
            total_seconds += join_seconds;
        }

        if( m_starttime.IsValid() ) {
            wxString s;
            if( m_bStartNow ) {
                wxDateTime d = wxDateTime::Now();
                if( tz_selection == 1 ) {
                    m_starttime = d.ToUTC();
                    s = _T(">");
                }
            }
            s += ts2s( m_starttime, tz_selection, (int) gStart_LMT_Offset, INPUT_FORMAT );
            m_StartTimeCtl->SetValue( s );
        } else
            m_StartTimeCtl->Clear();

        if( IsThisRouteExtendable() ) m_ExtendButton->Enable( true );

        //  Total length
        double total_length = m_pRoute->m_route_length;
        if( m_bStartNow ) {
            total_length += join_distance;
        }

        wxString slen;
        slen.Printf( wxT("%5.2f ") + getUsrDistanceUnit(), toUsrDistance( total_length ) );

        if( !m_pEnroutePoint ) m_TotalDistCtl->SetValue( slen );
        else
            m_TotalDistCtl->Clear();

        wxString time_form;
        wxString tide_form;

        //  Time
        wxTimeSpan time( 0, 0, (int) total_seconds, 0 );
        
        if(m_bcompact){
            if( total_seconds > 3600. * 24. )
                time_form = time.Format( _(" %D D  %H H  %M M") );
            else
                if( total_seconds > 0. )
                    time_form = time.Format( _(" %H H  %M M") );
                else
                    time_form = _T("--");
        }
        else{
            if( total_seconds > 3600. * 24. )
                time_form = time.Format( _(" %D Days  %H Hours  %M Minutes") );
            else
                if( total_seconds > 0. )
                    time_form = time.Format( _(" %H Hours  %M Minutes") );
                else
                    time_form = _T("--");
        }
        

        if( !m_pEnroutePoint )
            m_TimeEnrouteCtl->SetValue( time_form );
        else
            m_TimeEnrouteCtl->Clear();

        //  Iterate on Route Points
        wxRoutePointListNode *node = m_pRoute->pRoutePointList->GetFirst();

        int i = 0;
        double slat = gLat;
        double slon = gLon;
        double tdis = 0.;
        double tsec = 0.;    // total time in seconds

        int stopover_count = 0;
        bool arrival = true; // marks which pass over the wpt we do - 1. arrival 2. departure
        bool enroute = true; // for active route, skip all points up to the active point

        if( m_pRoute->m_bRtIsActive ) {
            if( m_pEnroutePoint && m_bStartNow )
                enroute = ( m_pRoute->GetPoint( 1 )->m_GUID == m_pEnroutePoint->m_GUID );
        }

        wxString nullify = _T("----");

        int i_prev_point = -1;
        RoutePoint *prev_route_point = NULL;

        while( node ) {
            RoutePoint *prp = node->GetData();
            long item_line_index = i + stopover_count;

            //  Leg
            wxString t;
            t.Printf( _T("%d"), i );
            if( i == 0 ) t = _T("---");
            if( arrival ) m_wpList->SetItem( item_line_index, cols[LEG], t );
/*
            if( arrival ){
                wxListItem item;
                item.SetId(i);
                item.SetColumn(0);
                item.SetText(t);
                m_wpList->SetItem(item);
            }
            
            m_wpList->SetColumnWidth( 0, wxLIST_AUTOSIZE );
            */
            
            //  Mark Name
            if( arrival ) m_wpList->SetItem( item_line_index, cols[TO_WAYPOINT], prp->GetName() );
        // Store Dewcription
            if( arrival ) m_wpList->SetItem( item_line_index, cols[DESCRIPTION], prp->GetDescription() );

            //  Distance
            //  Note that Distance/Bearing for Leg 000 is as from current position

            double brg, leg_dist;
            bool starting_point = false;

            starting_point = ( i == 0 ) && enroute;
            if( m_pEnroutePoint && !starting_point )
                starting_point = ( prp->m_GUID == m_pEnroutePoint->m_GUID );

            if( starting_point ) {
                slat = gLat;
                slon = gLon;
                if( gSog > 0.0 ) leg_speed = gSog; // should be VMG
                else
                    leg_speed = m_planspeed;
                if( m_bStartNow ) {
                    DistanceBearingMercator( prp->m_lat, prp->m_lon, slat, slon, &brg, &leg_dist );
                    joining_time = wxTimeSpan::Seconds( (long) wxRound( ( leg_dist * 3600. ) / leg_speed ) );
                }
                enroute = true;
            } else {
                if( prp->m_seg_vmg > 0. )
                    leg_speed = prp->m_seg_vmg;
                else
                    leg_speed = m_planspeed;
            }

            DistanceBearingMercator( prp->m_lat, prp->m_lon, slat, slon, &brg, &leg_dist );

            // calculation of course at current WayPoint.
            double course=10, tmp_leg_dist=23;
            wxRoutePointListNode *next_node = node->GetNext();
            RoutePoint * _next_prp = (next_node)? next_node->GetData(): NULL;
            if (_next_prp )
            {
            DistanceBearingMercator( _next_prp->m_lat, _next_prp->m_lon, prp->m_lat, prp->m_lon, &course, &tmp_leg_dist );
            }else
            {
              course = 0.0;
              tmp_leg_dist = 0.0;
            }

            prp->SetCourse(course); // save the course to the next waypoint for printing.
            // end of calculation


            t.Printf( _T("%6.2f ") + getUsrDistanceUnit(), toUsrDistance( leg_dist ) );
            if( arrival )
                m_wpList->SetItem( item_line_index, cols[DISTANCE], t );
            if( !enroute )
                m_wpList->SetItem( item_line_index, cols[DISTANCE], nullify );
            prp->SetDistance(leg_dist); // save the course to the next waypoint for printing.

                //  Bearing
            if( g_bShowTrue ) {
                if( enroute )
                    t.Printf( _T("%03.0f Deg. T"), brg );
                else
                    t = nullify;
                if( arrival )
                    m_wpList->SetItem( item_line_index, cols[BEARING], t );
            }

            if( g_bShowMag ){
                if( enroute ) {
                    double latAverage = (prp->m_lat + slat)/2;
                    double lonAverage = (prp->m_lon + slon)/2;
                    double varBrg = gFrame->GetMag( brg, latAverage, lonAverage);
                
                    t.Printf( _T("%03.0f Deg. M"), varBrg );
                } else
                    t = nullify;

                if( arrival )
                    m_wpList->SetItem( item_line_index, cols[BEARING_MAGNETIC], t );
            }



            // Course (bearing of next )
            if (_next_prp){
                if( g_bShowMag ){
                    if ( arrival ) {
                        double next_lat = prp->m_lat;
                        double next_lon = prp->m_lon;
                        if (_next_prp ){
                            next_lat = _next_prp->m_lat;
                            next_lon = _next_prp->m_lon;
                        }

                        double latAverage = (prp->m_lat + next_lat)/2;
                        double lonAverage = (prp->m_lon + next_lon)/2;
                        double varCourse = gFrame->GetMag( course, latAverage, lonAverage);

                        t.Printf( _T("%03.0f Deg. M"), varCourse );
                        m_wpList->SetItem( item_line_index, cols[COURSE_MAGNETIC], t );
                    }
                    else
                        m_wpList->SetItem( item_line_index, cols[COURSE_MAGNETIC], nullify );
                }
                if ( g_bShowTrue ) {
                    if ( arrival ) {
                        t.Printf( _T("%03.0f Deg. T"), course );
                        m_wpList->SetItem( item_line_index, cols[COURSE], t );
                    } else
                        m_wpList->SetItem( item_line_index, cols[COURSE], nullify );
                }
            }

            //  Lat/Lon
            wxString tlat = toSDMM( 1, prp->m_lat, false );  // low precision for routes
            if( arrival ) m_wpList->SetItem( item_line_index, cols[LATITUDE], tlat );

            wxString tlon = toSDMM( 2, prp->m_lon, false );
            if( arrival ) m_wpList->SetItem( item_line_index, cols[LONGITUDE], tlon );


            tide_form = _T("");

            LMT_Offset = long( ( prp->m_lon ) * 3600. / 15. );

            // Time to each waypoint or creation date for tracks
            if( i == 0 && enroute ) {
                time_form.Printf( _("Start") );
                if( m_starttime.IsValid() ) {
                    wxDateTime act_starttime = m_starttime + joining_time;
                    time_form.Append( _T(": ") );

                    if( !arrival ) {
                        wxDateTime etd = prp->m_seg_etd;
                        if( etd.IsValid() && etd.IsLaterThan( m_starttime ) ) {
                            stopover_time += etd.Subtract( m_starttime );
                            act_starttime = prp->m_seg_etd;
                        }
                    }

                    int ds = getDaylightStatus( prp->m_lat, prp->m_lon, act_starttime );
                    wxString s = ts2s( act_starttime, tz_selection, (int) LMT_Offset,
                            DISPLAY_FORMAT );
                    time_form.Append( s );
                    time_form.Append( _T("   (") );
                    time_form.Append( GetDaylightString(ds) );
                    time_form.Append( _T(")") );

                    if( ptcmgr ) {
                        int jx = 0;
                        if( prp->GetName().Find( _T("@~~") ) != wxNOT_FOUND ) {
                            tide_form = prp->GetName().Mid( prp->GetName().Find( _T("@~~") ) + 3 );
                            jx = ptcmgr->GetStationIDXbyName( tide_form, prp->m_lat, prp->m_lon );
                        }
                        if( gpIDX || jx ) {
                            time_t tm = act_starttime.GetTicks();
                            tide_form = MakeTideInfo( jx, tm, tz_selection, LMT_Offset );
                        }
                    }
                }
                tdis = 0;
                tsec = 0.;
            } // end of route point 0
            else {
                if( arrival && enroute ) tdis += leg_dist;
                if( leg_speed ) {
                    if( arrival && enroute ) tsec += 3600 * leg_dist / leg_speed; // time in seconds to arrive here
                    wxTimeSpan time( 0, 0, (int) tsec, 0 );

                    if( m_starttime.IsValid() ) {

                        wxDateTime ueta = m_starttime;
                        ueta.Add( time + stopover_time + joining_time );

                        if( !arrival ) {
                            wxDateTime etd = prp->m_seg_etd;
                            if( etd.IsValid() && etd.IsLaterThan( ueta ) ) {
                                stopover_time += etd.Subtract( ueta );
                                ueta = prp->m_seg_etd;
                            }
                        }

                        int ds = getDaylightStatus( prp->m_lat, prp->m_lon, ueta );
                        time_form = ts2s( ueta, tz_selection, LMT_Offset, DISPLAY_FORMAT );
                        time_form.Append( _T("   (") );
                        time_form.Append( GetDaylightString(ds) );
                        time_form.Append( _T(")") );


                        if( ptcmgr ) {
                            int jx = 0;
                            if( prp->GetName().Find( _T("@~~") ) != wxNOT_FOUND ) {
                                tide_form = prp->GetName().Mid(
                                        prp->GetName().Find( _T("@~~") ) + 3 );
                                jx = ptcmgr->GetStationIDXbyName( tide_form, prp->m_lat, prp->m_lon );
                            }
                            if( gpIDX || jx ) {
                                time_t tm = ueta.GetTicks();
                                tide_form = MakeTideInfo( jx, tm, tz_selection, LMT_Offset );
                            }
                        }
                    } else {
                        if( tsec > 3600. * 24. ) time_form = time.Format( _T(" %D D  %H H  %M M") );
                        else
                            time_form = time.Format( _T(" %H H  %M M") );
                    }
                } // end if legspeed
            }  // end of repeatable route point

            if( enroute && ( arrival || m_starttime.IsValid() ) ) m_wpList->SetItem(
                item_line_index, cols[ETE_ETD], time_form );
            else
                m_wpList->SetItem( item_line_index, cols[ETE_ETD], _T("----") );

            //Leg speed
            wxString s;
            s.Printf( _T("%5.2f"), toUsrSpeed( leg_speed ) );
            if( arrival ) m_wpList->SetItem( item_line_index, cols[SPEED], s );

            if( enroute ) m_wpList->SetItem( item_line_index, cols[NEXT_TIDE], tide_form );
            else {
                m_wpList->SetItem( item_line_index, cols[NEXT_TIDE], nullify );
                m_wpList->SetItem( item_line_index, cols[DESCRIPTION], nullify );
            }

            //  Save for iterating distance/bearing calculation
            slat = prp->m_lat;
            slon = prp->m_lon;

            // if stopover (ETD) found, loop for next output line for the same point
            //   with departure time & tide information

            if( arrival && ( prp->m_seg_etd.IsValid() ) ) {
                stopover_count++;
                arrival = false;
            } else {
                arrival = true;
                i++;
                node = node->GetNext();

                //    Record this point info for use as previous point in next iteration.
                i_prev_point = i - 1;
                prev_route_point = prp;
            }
        }
    }

    if( m_pRoute->m_Colour == wxEmptyString ) m_chColor->Select( 0 );
    else {
        for( unsigned int i = 0; i < sizeof( ::GpxxColorNames ) / sizeof(wxString); i++ ) {
            if( m_pRoute->m_Colour == ::GpxxColorNames[i] ) {
                m_chColor->Select( i + 1 );
                break;
            }
        }
    }

    for( unsigned int i = 0; i < sizeof( ::StyleValues ) / sizeof(int); i++ ) {
        if( m_pRoute->m_style == ::StyleValues[i] ) {
            m_chStyle->Select( i );
            break;
        }
    }

    for( unsigned int i = 0; i < sizeof( ::WidthValues ) / sizeof(int); i++ ) {
        if( m_pRoute->m_width == ::WidthValues[i] ) {
            m_chWidth->Select( i );
            break;
        }
    }

    ::wxEndBusyCursor();

    m_wpList->SetColumnWidth( 0, wxLIST_AUTOSIZE );
    
    return true;
}

wxString RouteProp::MakeTideInfo( int jx, time_t tm, int tz_selection, long LMT_Offset )
{
    int ev = 0;
    wxString tide_form;
    
    // tm comes in as UTC...
    // convert to local time for GetNextBigEvent
    wxDateTime now = wxDateTime::Now();
    wxDateTime nowutc = wxDateTime::Now().MakeUTC();
    time_t t_off = now.GetTicks() - nowutc.GetTicks();
    
    wxDateTime dtmu;
    dtmu.Set( tm + t_off );

    time_t dtmtt = dtmu.GetTicks();
    
    if( gpIDX ) {
        ev = ptcmgr->GetNextBigEvent( &dtmtt,
                ptcmgr->GetStationIDXbyName( wxString( gpIDX->IDX_station_name, wxConvUTF8 ),
                        gpIDX->IDX_lat, gpIDX->IDX_lon ) );
    } else
        ev = ptcmgr->GetNextBigEvent( &dtmtt, jx );

    //convert event time back to UTC
    wxDateTime dtm;
    dtm.Set( dtmtt ).MakeUTC(); 
    
    if( ev == 1 ) tide_form.Printf( _T("LW: ") );
    if( ev == 2 ) tide_form.Printf( _T("HW: ") );
    
    tide_form.Append( ts2s( dtm, tz_selection, LMT_Offset, DISPLAY_FORMAT ) );
    if( !gpIDX ) {
        wxString locn( ptcmgr->GetIDX_entry( jx )->IDX_station_name, wxConvUTF8 );
        tide_form.Append( _T(" @~~") );
        tide_form.Append( locn );
    }
    return ( tide_form );
}

bool RouteProp::SaveChanges( void )
{

    //  Save the current planning speed
    g_PlanSpeed = m_planspeed;
    g_StartTime = m_starttime;    // both always UTC
    g_StartTimeTZ = GetTZSelection();
    m_StartTimeCtl->Clear();

    if( m_pRoute && !m_pRoute->m_bIsInLayer ) {
        //  Get User input Text Fields
        m_pRoute->m_RouteNameString = m_RouteNameCtl->GetValue();
        m_pRoute->m_RouteStartString = m_RouteStartCtl->GetValue();
        m_pRoute->m_RouteEndString = m_RouteDestCtl->GetValue();
        if( m_chColor->GetSelection() == 0 ) m_pRoute->m_Colour = wxEmptyString;
        else
            m_pRoute->m_Colour = ::GpxxColorNames[m_chColor->GetSelection() - 1];
        m_pRoute->m_style = (wxPenStyle)::StyleValues[m_chStyle->GetSelection()];
        m_pRoute->m_width = ::WidthValues[m_chWidth->GetSelection()];
        m_pRoute->m_PlannedDeparture = g_StartTime;
        m_pRoute->m_PlannedSpeed = m_planspeed;
        switch( g_StartTimeTZ ) {
            case 1 :
                m_pRoute->m_TimeDisplayFormat = RTE_TIME_DISP_PC;
                break;
            case 2 :
                m_pRoute->m_TimeDisplayFormat = RTE_TIME_DISP_LOCAL;
                break;
            default:
                m_pRoute->m_TimeDisplayFormat = RTE_TIME_DISP_UTC;
        }

        pConfig->UpdateRoute( m_pRoute );
        pConfig->UpdateSettings();
    }

    return true;
}

void RouteProp::OnPlanSpeedCtlUpdated( wxCommandEvent& event )
{
    //  Fetch the value, and see if it is a "reasonable" speed
    wxString spd = m_PlanSpeedCtl->GetValue();
    double s;
    spd.ToDouble( &s );
    if( ( 0.1 < s ) && ( s < 1000.0 ) ) {
        m_planspeed = fromUsrSpeed( s );

        UpdateProperties();
    }

    event.Skip();
}

void RouteProp::OnStartTimeCtlUpdated( wxCommandEvent& event )
{
    //  Fetch the value, and see if it is a "reasonable" time
    wxString stime = m_StartTimeCtl->GetValue();
    int tz_selection = GetTZSelection();

    wxDateTime d;
    if( stime.StartsWith( _T(">") ) ) {
        if( m_pRoute->m_bRtIsActive ) {
            m_pEnroutePoint = g_pRouteMan->GetpActivePoint();
        }
        m_bStartNow = true;
        d = wxDateTime::Now();
        if( tz_selection == 1 ) m_starttime = d.ToUTC();
        else
            m_starttime = wxInvalidDateTime; // can't get it to work otherwise
    } else {
        m_pEnroutePoint = NULL;
        m_bStartNow = false;
        if( !d.ParseDateTime( stime ) )        // only specific times accepted
        d = wxInvalidDateTime;

        m_starttime = d;

        if( m_starttime.IsValid() ) {
            if( tz_selection == 1 ) m_starttime = d.ToUTC();
            if( tz_selection == 2 ) {
                wxTimeSpan glmt( 0, 0, (int) gStart_LMT_Offset, 0 );
                m_starttime -= glmt;
            }
        }
    }

    UpdateProperties();

    //    event.Skip();
}

void RouteProp::OnTimeZoneSelected( wxCommandEvent& event )
{
    UpdateProperties();

    event.Skip();
}

void RouteProp::OnRoutepropCancelClick( wxCommandEvent& event )
{
    //    Look in the route list to be sure the raoute is still available
    //    (May have been deleted by RouteMangerDialog...)

    bool b_found_route = false;
    wxRouteListNode *node = pRouteList->GetFirst();
    while( node ) {
        Route *proute = node->GetData();

        if( proute == m_pRoute ) {
            b_found_route = true;
            break;
        }
        node = node->GetNext();
    }

    if( b_found_route ) m_pRoute->ClearHighlights();

    m_bStartNow = false;

    #ifdef __WXGTK__ 
    gFrame->Raise();
    #endif
    Hide();
    gFrame->RefreshAllCanvas( false );

    event.Skip();
}

void RouteProp::OnRoutepropOkClick( wxCommandEvent& event )
{
    //    Look in the route list to be sure the route is still available
    //    (May have been deleted by RouteManagerDialog...)

    bool b_found_route = false;
    wxRouteListNode *node = pRouteList->GetFirst();
    while( node ) {
        Route *proute = node->GetData();

        if( proute == m_pRoute ) {
            b_found_route = true;
            break;
        }
        node = node->GetNext();
    }

    if( b_found_route ) {
        SaveChanges();              // write changes to globals and update config
        m_pRoute->ClearHighlights();
    }

    m_pEnroutePoint = NULL;
    m_bStartNow = false;

    if( RouteManagerDialog::getInstanceFlag() && pRouteManagerDialog->IsShown() )
        pRouteManagerDialog->UpdateRouteListCtrl();

    #ifdef __WXGTK__ 
    gFrame->Raise();
    #endif
    Hide();
    gFrame->InvalidateAllGL();
    gFrame->RefreshAllCanvas( false );

    event.Skip();

}

void RouteProp::OnEvtColDragEnd( wxListEvent& event )
{
    m_wpList->Refresh();
}


/*
 * Copyright 2017 Valve Software
 *
 * All Rights Reserved.
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
 * THE SOFTWARE.
 */
#include <stdio.h>
#include <string.h>

#include <algorithm>
#include <future>
#include <set>
#include <sys/stat.h>
#include <sys/types.h>
#include <unordered_map>
#include <vector>
#include <array>
#include <limits.h>
#include <functional>

#include <SDL.h>

#include "imgui/imgui.h"

#include "gpuvis_macros.h"
#include "stlini.h"
#include "trace-cmd/trace-read.h"
#include "gpuvis_utils.h"
#include "gpuvis.h"

/*
  **** TODO list... ****

  Check if entire rows are clipped when drawing...

  Configurable hotkeys?

  From Pierre-Loup:
    * Since I find myself zooming in and out a lot to situate myself in the
    larger trace and analyze parts of it, I'm thinking one of the next big
    challenges is going to find a way to have a little stripe at the top and
    bottom that somehow displays the whole trace and efficiently shows where
    you are in there. I have _no_ idea what data to surface there to show
    you the trace "at a glance". I'm going to guess that it'll vary a lot
    from one usecase to another, so maybe the answer is to be able to 'pin'
    a graph row of any type, which would then stay like 10x zoomed out
    compared to your real zoom level, and shows you where you are in there
    in the middle? Does that make any sense?
*/

/*
  From conversations with Andres and Pierre-Loup...

  These are the important events:

  amdgpu_cs_ioctl:
    this event links a userspace submission with a kernel job
    it appears when a job is received from userspace
    dictates the userspace PID for the whole unit of work
      ie, the process that owns the work executing on the gpu represented by the bar
    only event executed within the context of the userspace process

  amdgpu_sched_run_job:
    links a job to a dma_fence object, the queue into the HW event
    start of the bar in the gpu timeline; either right now if no job is running, or when the currently running job finishes

  *fence_signaled:
    job completed
    dictates the end of the bar

  notes:
    amdgpu_cs_ioctl and amdgpu_sched_run_job have a common job handle

  We want to match: timeline, context, seqno.

    There are separate timelines for each gpu engine
    There are two dma timelines (one per engine)
    And 8 compute timelines (one per hw queue)
    They are all concurrently executed
      Most apps will probably only have a gfx timeline
      So if you populate those lazily it should avoid clogging the ui

  Andres warning:
    btw, expect to see traffic on some queues that was not directly initiated by an app
    There is some work the kernel submits itself and that won't be linked to any cs_ioctl

  Example:

  ; userspace submission
    SkinningApp-2837 475.1688: amdgpu_cs_ioctl:      sched_job=185904, timeline=gfx, context=249, seqno=91446, ring_name=ffff94d7a00d4694, num_ibs=3

  ; gpu starting job
            gfx-477  475.1689: amdgpu_sched_run_job: sched_job=185904, timeline=gfx, context=249, seqno=91446, ring_name=ffff94d7a00d4694, num_ibs=3

  ; job completed
         <idle>-0    475.1690: fence_signaled:       driver=amd_sched timeline=gfx context=249 seqno=91446
 */

class event_renderer_t
{
public:
    event_renderer_t( float y_in, float w_in, float h_in );

    void add_event( float x );
    void done();

    void set_y( float y_in, float h_in );

protected:
    void start( float x );
    void draw();

public:
    float x0, x1;
    uint32_t num_events;

    float y, w, h;
};

typedef std::function< uint32_t ( class graph_info_t &gi ) > RenderGraphRowCallback;

struct row_info_t
{
    uint32_t id;
    std::string row_name;

    uint32_t num_events = 0;
    float minval = FLT_MAX;
    float maxval = FLT_MIN;

    float row_y;
    float row_h;

    TraceEvents::loc_type_t row_type;
    const std::vector< uint32_t > *plocs;

    RenderGraphRowCallback render_cb;
};

class graph_info_t
{
public:
    void init_row_info( TraceWin *win, const std::vector< GraphRows::graph_rows_info_t > &graph_rows );

    void init( TraceWin *win, float x, float w );
    void set_pos_y( float y, float h, row_info_t *ri );

    float ts_to_x( int64_t ts );
    float ts_to_screenx( int64_t ts );

    int64_t screenx_to_ts( float x_in );
    int64_t dx_to_ts( float x_in );

    bool pt_in_graph( const ImVec2 &posin );
    bool mouse_pos_in_graph();
    bool mouse_pos_in_rect( float x, float w, float y, float h );

    row_info_t *find_row( const char *name );

    bool add_mouse_hovered_event( float x, const trace_event_t &event );

public:
    float x, y, w, h;

    int64_t ts0;
    int64_t ts1;
    int64_t tsdx;
    double tsdxrcp;

    uint32_t eventstart;
    uint32_t eventend;

    bool mouse_over;
    ImVec2 mouse_pos;

    struct hovered_t
    {
        bool neg;
        int64_t dist_ts;
        uint32_t eventid;
    };
    const size_t hovered_max = 6;
    std::vector< hovered_t > hovered_items;

    // Id of hovered / selected fence signaled event
    uint32_t hovered_fence_signaled = INVALID_ID;

    bool timeline_render_user;
    bool graph_only_filtered;

    std::vector< row_info_t > row_info;
    row_info_t *prinfo_cur = nullptr;
    row_info_t *prinfo_zoom = nullptr;
    row_info_t *prinfo_zoom_hw = nullptr;

    float text_h;
    float row_h;
    float visible_graph_height;
    float total_graph_height;
};

static void imgui_drawrect( float x, float w, float y, float h, ImU32 color )
{
    if ( w < 0.0f )
    {
        x += w;
        w = -w;
    }

    if ( w <= 1.0f )
        ImGui::GetWindowDrawList()->AddLine( ImVec2( x, y - 0.5f ), ImVec2( x, y + h - 0.5f ), color );
    else
        ImGui::GetWindowDrawList()->AddRectFilled( ImVec2( x, y ), ImVec2( x + w, y + h ), color );
}

static void imgui_draw_text( float x, float y, const char *text, ImU32 color, bool draw_background = false )
{
    if ( draw_background )
    {
        ImVec2 textsize = ImGui::CalcTextSize( text );

        ImGui::GetWindowDrawList()->AddRectFilled(
                    ImVec2( x - 1, y - 1 ), ImVec2( x + textsize.x + 2, y + textsize.y + 2 ),
                    s_clrs().get( col_Graph_RowLabelTextBk ) );
    }

    ImGui::GetWindowDrawList()->AddText( ImVec2( x, y ), color, text );
}

const char *get_event_field_val( const trace_event_t &event, const char *name )
{
    for ( const event_field_t &field : event.fields )
    {
        if ( !strcmp( field.key, name ) )
            return field.value;
    }

    return "";
}

/*
 * event_renderer_t
 */
event_renderer_t::event_renderer_t( float y_in, float w_in, float h_in )
{
    y = y_in;
    w = w_in;
    h = h_in;

    start( -1.0f );
}

void event_renderer_t::set_y( float y_in, float h_in )
{
    if ( y != y_in || h != h_in )
    {
        done();

        y = y_in;
        h = h_in;
    }
}

void event_renderer_t::add_event( float x )
{
    if ( x0 < 0.0f )
    {
        // First event
        start( x );
    }
    else if ( x - x1 <= 1.0f )
    {
        // New event real close to last event
        x1 = x;
        num_events++;
    }
    else
    {
        // New event is away from current group, so draw.
        draw();

        // Start a new group
        start( x );
    }
}

void event_renderer_t::done()
{
    if ( x0 != -1 )
    {
        draw();
        start( -1.0f );
    }
}

void event_renderer_t::start( float x )
{
    num_events = 0;
    x0 = x;
    x1 = x + .0001f;
}

void event_renderer_t::draw()
{
    int index = std::min< int >( col_Graph_1Event + num_events, col_Graph_6Event );
    ImU32 color = s_clrs().get( index );
    float min_width = std::min< float >( num_events + 1.0f, 4.0f );
    float width = std::max< float >( x1 - x0, min_width );

    imgui_drawrect( x0, width, y, h, color );
}

static option_id_t get_comm_option_id( TraceLoader &loader, const std::string &row_name )
{
    option_id_t optid = s_opts().get_opt_graph_rowsize_id( row_name );

    if ( optid != OPT_Invalid )
        return optid;

    if ( !strncmp( row_name.c_str(), "plot:", 5 ) )
        return s_opts().add_opt_graph_rowsize( row_name.c_str() );

    return OPT_Invalid;
}

/*
 * graph_info_t
 */
void graph_info_t::init_row_info( TraceWin *win, const std::vector< GraphRows::graph_rows_info_t > &graph_rows )
{
    uint32_t id = 0;

    imgui_push_smallfont();

    float graph_row_padding = ImGui::GetStyle().FramePadding.y;

    text_h = ImGui::GetTextLineHeightWithSpacing();
    row_h = text_h * 2 + graph_row_padding;

    total_graph_height = graph_row_padding;

    imgui_pop_smallfont();

    for ( const GraphRows::graph_rows_info_t &grow : graph_rows )
    {
        row_info_t rinfo;
        option_id_t optid = OPT_Invalid;
        const std::vector< uint32_t > *plocs;
        const std::string &row_name = grow.row_name;

        if ( grow.hidden )
            continue;

        plocs = win->m_trace_events.get_locs( row_name.c_str(), &rinfo.row_type );

        rinfo.row_y = total_graph_height;
        rinfo.row_h = text_h * 2;
        rinfo.row_name = row_name;

        if ( !plocs )
        {
            // Nothing to render
            rinfo.render_cb = nullptr;
        }
        else if ( rinfo.row_type == TraceEvents::LOC_TYPE_Print )
        {
            // ftrace print row
            optid = get_comm_option_id( win->m_loader, rinfo.row_name );
            rinfo.render_cb = std::bind( &TraceWin::graph_render_print_timeline, win, _1 );
        }
        else if ( rinfo.row_type == TraceEvents::LOC_TYPE_Plot )
        {
            optid = get_comm_option_id( win->m_loader, rinfo.row_name );
            rinfo.render_cb = std::bind( &TraceWin::graph_render_plot, win, _1 );
        }
        else if ( rinfo.row_type == TraceEvents::LOC_TYPE_Timeline )
        {
            optid = get_comm_option_id( win->m_loader, rinfo.row_name );
            rinfo.render_cb = std::bind( &TraceWin::graph_render_row_timeline, win, _1 );
        }
        else if ( rinfo.row_type == TraceEvents::LOC_TYPE_Timeline_hw )
        {
            rinfo.row_h = 2 * text_h;
            rinfo.render_cb = std::bind( &TraceWin::graph_render_hw_row_timeline, win, _1 );
        }
        else
        {
            // LOC_Type_Comm or LOC_TYPE_Tdopexpr hopefully
            rinfo.render_cb = std::bind( &TraceWin::graph_render_row_events, win, _1 );
        }

        if ( optid != OPT_Invalid )
        {
            int rows = ( optid != OPT_Invalid ) ? s_opts().geti( optid ) : 4;

            rinfo.row_h = Clamp< int >( rows, 2, 50 ) * text_h;
        }

        rinfo.id = id++;
        rinfo.plocs = plocs;
        row_info.push_back( rinfo );

        total_graph_height += rinfo.row_h + graph_row_padding;
    }

    total_graph_height += imgui_scale( 2.0f );
    total_graph_height = std::max< float >( total_graph_height, 4 * row_h );
}

void graph_info_t::init( TraceWin *win, float x_in, float w_in )
{
    x = x_in;
    w = w_in;

    ts0 = win->m_graph.start_ts + win->m_eventlist.tsoffset;
    ts1 = ts0 + win->m_graph.length_ts;

    eventstart = win->ts_to_eventid( ts0 );
    eventend = win->ts_to_eventid( ts1 );

    tsdx = ts1 - ts0 + 1;
    tsdxrcp = 1.0 / tsdx;

    mouse_pos = ImGui::IsRootWindowOrAnyChildFocused() ?
                ImGui::GetMousePos() : ImGui::GetIO().MousePosInvalid;

    // Check if we're supposed to render filtered events only
    graph_only_filtered = s_opts().getb( OPT_GraphOnlyFiltered ) &&
                          !win->m_eventlist.filtered_events.empty();

    timeline_render_user = s_opts().getb( OPT_TimelineRenderUserSpace );

    const std::vector< trace_event_t > &events = win->m_trace_events.m_events;

    // First check if they're hovering a timeline event in the event list
    uint32_t event_hov = win->m_eventlist.hovered_eventid;

    // If not, check if they're hovering a timeline event in the graph
    if ( !is_valid_id( event_hov ) || !events[ event_hov ].is_timeline() )
        event_hov = win->m_graph.hovered_eventid;

    if ( is_valid_id( event_hov ) && events[ event_hov ].is_timeline() )
    {
        // Find the fence signaled event for this timeline
        std::string context = get_event_gfxcontext_str( events[ event_hov ] );
        const std::vector< uint32_t > *plocs = win->m_trace_events.get_gfxcontext_locs( context.c_str() );

        // Mark it as hovered so it'll have a selection rectangle
        hovered_fence_signaled = plocs->back();
    }
}

void graph_info_t::set_pos_y( float y_in, float h_in, row_info_t *ri )
{
    y = y_in;
    h = h_in;

    prinfo_cur = ri;

    mouse_over = mouse_pos.x >= x &&
            mouse_pos.x <= x + w &&
            mouse_pos.y >= y &&
            mouse_pos.y <= y + h;
}

float graph_info_t::ts_to_x( int64_t ts )
{
    return w * ( ts - ts0 ) * tsdxrcp;
}

float graph_info_t::ts_to_screenx( int64_t ts )
{
    return x + ts_to_x( ts );
}

int64_t graph_info_t::screenx_to_ts( float x_in )
{
    double val = ( x_in - x ) / w;

    return ts0 + val * tsdx;
}
int64_t graph_info_t::dx_to_ts( float x_in )
{
    return ( x_in / w ) * tsdx;
}

bool graph_info_t::pt_in_graph( const ImVec2 &posin )
{
    return ( posin.x >= x && posin.x <= x + w &&
             posin.y >= y && posin.y <= y + h );
}

bool graph_info_t::mouse_pos_in_graph()
{
    return pt_in_graph( mouse_pos );
}

bool graph_info_t::mouse_pos_in_rect( float x0, float width, float y0, float height )
{
    return ( mouse_pos.x >= x0 &&
             mouse_pos.x <= x0 + width &&
             mouse_pos.y >= y0 &&
             mouse_pos.y <= y0 + height );
}

row_info_t *graph_info_t::find_row( const char *name )
{
    for ( row_info_t &ri : row_info )
    {
        if ( ri.row_name == name )
            return &ri;
    }
    return NULL;
}

bool graph_info_t::add_mouse_hovered_event( float xin, const trace_event_t &event )
{
    bool inserted = false;
    float xdist_mouse = xin - mouse_pos.x;
    bool neg = xdist_mouse < 0.0f;

    if ( neg )
        xdist_mouse = -xdist_mouse;

    if ( xdist_mouse < imgui_scale( 8.0f ) )
    {
        int64_t dist_ts = dx_to_ts( xdist_mouse );

        for ( auto it = hovered_items.begin(); it != hovered_items.end(); it++ )
        {
            if ( dist_ts < it->dist_ts )
            {
                hovered_items.insert( it, { neg, dist_ts, event.id } );
                inserted = true;
                break;
            }
        }

        if ( !inserted && ( hovered_items.size() < hovered_max ) )
        {
            hovered_items.push_back( { neg, dist_ts, event.id } );
            inserted = true;
        }
        else if ( hovered_items.size() > hovered_max )
        {
            hovered_items.pop_back();
        }
    }

    return inserted;
}

static size_t str_get_digit_loc( const char *str )
{
    const char *buf = str;

    for ( ; *buf; buf++ )
    {
        if ( isdigit( *buf ) )
            return buf - str;
    }

    return 0;
}

const char *CreatePlotDlg::get_plot_str( const trace_event_t &event )
{
    if ( event.is_ftrace_print() )
    {
        const char *buf = get_event_field_val( event, "buf" );

        if ( str_get_digit_loc( buf ) )
            return buf;
    }

    return NULL;
}

bool CreatePlotDlg::init( TraceEvents &trace_events, uint32_t eventid )
{
    m_plot = NULL;
    m_plot_name = "";

    if ( !is_valid_id( eventid ) )
        return false;

    const trace_event_t &event = trace_events.m_events[ eventid ];
    const char *buf = get_event_field_val( event, "buf" );
    size_t digit_loc = str_get_digit_loc( buf );

    m_plot_buf = buf;
    m_plot_err_str.clear();

    /*
           [Compositor] NewFrame idx=2776
           [Compositor Client] WaitGetPoses End ThreadId=5125
           [Compositor] frameTimeout( 27 ms )
           [Compositor Client] Received Idx 100
           [Compositor] NewFrame idx=3769
           [Compositor] Predicting( 33.047485 ms )
           [Compositor] Re-predicting( 25.221056 ms )
           [Compositor] Re-predicting( -28.942781 ms )
           [Compositor] TimeSinceLastVSync: 0.076272(79975)
        */
    if ( digit_loc )
    {
        std::string shortstr;
        std::string fullstr = string_ltrimmed( std::string( buf, digit_loc ) );

        // Skip the [Blah blah] section for the plot name
        if ( fullstr[ 0 ] == '[' )
        {
            char *right_bracket = strchr( &fullstr[ 0 ], ']' );

            if ( right_bracket )
                shortstr = std::string( right_bracket + 1 );
        }
        if ( shortstr.empty() )
            shortstr = fullstr;

        std::string namestr = string_trimmed( string_remove_punct( shortstr ) );
        strcpy_safe( m_plot_name_buf, namestr.c_str() );

        std::string filter_str = string_format( "$buf =~ \"%s\"", fullstr.c_str() );
        strcpy_safe( m_plot_filter_buf, filter_str.c_str() );

        fullstr += "%f";
        strcpy_safe( m_plot_scanf_buf, fullstr.c_str() );

        ImGui::OpenPopup( "Create Plot" );
        return true;
    }

    return false;
}

template < size_t T >
static void plot_input_text( const char *label, char ( &buf )[ T ], float x, float w, ImGuiTextEditCallback callback = nullptr )
{
    ImGuiInputTextFlags flags = callback ? ImGuiInputTextFlags_CallbackCharFilter : 0;

    ImGui::PushID( label );

    ImGui::AlignFirstTextHeightToWidgets();
    ImGui::Text( "%s", label );

    ImGui::SameLine();
    ImGui::PushItemWidth( w );
    ImGui::SetCursorPos( { x, ImGui::GetCursorPos().y } );
    ImGui::InputText( "##plot_input_text", buf, sizeof( buf ), flags, callback );
    ImGui::PopItemWidth();

    ImGui::PopID();
}

bool CreatePlotDlg::render_dlg( TraceEvents &trace_events )
{
    if ( !ImGui::BeginPopupModal( "Create Plot", NULL, ImGuiWindowFlags_AlwaysAutoResize ) )
        return false;

    ParsePlotStr parse_plot_str;
    float w = imgui_scale( 350.0f );
    const ImVec2 button_size = { imgui_scale( 120.0f ), 0.0f };
    const ImVec2 text_size = ImGui::CalcTextSize( "Plot Scan Str: " );
    float x = ImGui::GetCursorPos().x + text_size.x;

    if ( parse_plot_str.init( m_plot_scanf_buf ) &&
         parse_plot_str.parse( m_plot_buf.c_str() ) )
    {
        const char *buf = m_plot_buf.c_str();
        const char *val_start = parse_plot_str.m_val_start;
        const char *val_end = parse_plot_str.m_val_end;
        int buf_len = ( int )( val_start - buf );
        int val_len = ( int )( val_end - val_start );

        ImGui::Text( "%s%.*s%s%.*s%s%s",
                     s_textclrs().str( TClr_Bright ), buf_len, buf,
                     s_textclrs().str( TClr_BrightComp ), val_len, val_start,
                     s_textclrs().str( TClr_Bright ), val_end );
    }
    else
    {
        ImGui::TextColored( s_clrs().getv4( col_BrightText ), "%s", m_plot_buf.c_str() );
    }

    ImGui::NewLine();

    struct PlotNameFilter {
        static int FilterPunct( ImGuiTextEditCallbackData *data )
                { return ( ( data->EventChar < 256 ) && ispunct( data->EventChar ) ); }
    };
    plot_input_text( "Plot Name:", m_plot_name_buf, x, w, PlotNameFilter::FilterPunct );

    plot_input_text( "Plot Filter:", m_plot_filter_buf, x, w );

    if ( m_plot_err_str.size() )
        ImGui::TextColored( ImVec4( 1, 0, 0, 1), "%s", m_plot_err_str.c_str() );

    plot_input_text( "Plot Scan Str:", m_plot_scanf_buf, x, w );

    ImGui::NewLine();

    bool disabled = !m_plot_name_buf[ 0 ] || !m_plot_filter_buf[ 0 ] || !m_plot_scanf_buf[ 0 ];
    if ( disabled )
        ImGui::PushStyleColor( ImGuiCol_Text, ImGui::GetColorVec4( ImGuiCol_TextDisabled ) );

    if ( ImGui::Button( "Create", button_size ) && !disabled )
    {
        m_plot_err_str.clear();
        const std::vector< uint32_t > *plocs = trace_events.get_tdopexpr_locs(
                    m_plot_filter_buf, &m_plot_err_str );

        if ( !plocs && m_plot_err_str.empty() )
        {
            m_plot_err_str = "WARNING: No events found.";
        }
        else
        {
            m_plot_name = std::string( "plot:" ) + m_plot_name_buf;

            GraphPlot &plot = trace_events.get_plot( m_plot_name.c_str() );

            if ( plot.init( trace_events, m_plot_name,
                            m_plot_filter_buf, m_plot_scanf_buf ) )
            {
                m_plot = &plot;
                ImGui::CloseCurrentPopup();
            }
            else
            {
                m_plot_err_str = "WARNING: No plot data values found.";
            }
        }
    }

    if ( disabled )
        ImGui::PopStyleColor();

    ImGui::SameLine();
    if ( ImGui::Button( "Cancel", button_size ) || imgui_key_pressed( ImGuiKey_Escape ) )
        ImGui::CloseCurrentPopup();

    ImGui::EndPopup();

    return !!m_plot;
}

void CreatePlotDlg::add_plot( GraphRows &rows )
{
    if ( rows.find_row( m_plot_name ) == ( size_t )-1 )
    {
        size_t print_row_index = rows.find_row( "print", rows.m_graph_rows_list.size() - 1 );
        auto it = rows.m_graph_rows_list.begin() + print_row_index + 1;

        rows.m_graph_rows_list.insert( it,
                { TraceEvents::LOC_TYPE_Plot, m_plot->m_plotdata.size(), m_plot_name, false } );
    }

    std::string val = string_format( "%s\t%s", m_plot->m_filter_str.c_str(), m_plot->m_scanf_str.c_str() );
    s_ini().PutStr( m_plot_name.c_str(), val.c_str(), "$graph_plots$" );
}

bool GraphPlot::init( TraceEvents &trace_events, const std::string &name,
                      const std::string &filter_str, const std::string scanf_str )
{
    m_name = name;
    m_filter_str = filter_str;
    m_scanf_str = scanf_str;

    m_minval = FLT_MAX;
    m_maxval = FLT_MIN;
    m_plotdata.clear();

    std::string errstr;
    const std::vector< uint32_t > *plocs = trace_events.get_tdopexpr_locs( m_filter_str.c_str(), &errstr );

    if ( plocs )
    {
        if ( scanf_str == "$duration" )
        {
            for ( uint32_t idx : *plocs )
            {
                const trace_event_t &event = trace_events.m_events[ idx ];

                float valf = event.duration * ( 1.0 / NSECS_PER_MSEC );

                m_minval = std::min< float >( m_minval, valf );
                m_maxval = std::max< float >( m_maxval, valf );

                m_plotdata.push_back( { event.ts, event.id, valf } );
            }
        }
        else
        {
            ParsePlotStr parse_plot_str;

            if ( parse_plot_str.init( m_scanf_str.c_str() ) )
            {
                for ( uint32_t idx : *plocs )
                {
                    const trace_event_t &event = trace_events.m_events[ idx ];
                    const char *buf = get_event_field_val( event, "buf" );

                    if ( parse_plot_str.parse( buf ) )
                    {
                        float valf = parse_plot_str.m_valf;

                        m_minval = std::min< float >( m_minval, valf );
                        m_maxval = std::max< float >( m_maxval, valf );

                        m_plotdata.push_back( { event.ts, event.id, valf } );
                    }
                }
            }
        }
    }

    return !m_plotdata.empty();
}

uint32_t GraphPlot::find_ts_index( int64_t ts0 )
{
    auto lambda = []( const GraphPlot::plotdata_t &lhs, int64_t ts )
                            { return lhs.ts < ts; };
    auto i = std::lower_bound( m_plotdata.begin(), m_plotdata.end(), ts0, lambda );

    if ( i != m_plotdata.end() )
    {
        size_t index = i - m_plotdata.begin();

        return ( index > 0 ) ? ( index - 1 ) : 0;
    }

    return ( uint32_t )-1;
}

bool ParsePlotStr::init( const char *scanf_str )
{
    const char *pct_f = strstr( scanf_str, "%f" );

    if ( pct_f )
    {
        m_scanf_str = scanf_str;
        m_scanf_len = pct_f - scanf_str;
        return true;
    }

    return false;
}

bool ParsePlotStr::parse( const char *buf )
{
    if ( buf )
    {
        const char *pat_start = strncasestr( buf, m_scanf_str, m_scanf_len );

        if ( pat_start )
        {
            char *val_end;
            const char *val_start = pat_start + m_scanf_len;

            m_valf = strtof( val_start, &val_end );

            if ( val_start != val_end )
            {
                m_val_start = val_start;
                m_val_end = val_end;
                return true;
            }
        }
    }

    return false;
}

uint32_t TraceWin::graph_render_plot( graph_info_t &gi )
{
    float minval = FLT_MAX;
    float maxval = FLT_MIN;
    std::vector< ImVec2 > points;
    const char *row_name = gi.prinfo_cur->row_name.c_str();
    GraphPlot &plot = m_trace_events.get_plot( row_name );
    uint32_t index0 = plot.find_ts_index( gi.ts0 );
    uint32_t index1 = plot.find_ts_index( gi.ts1 );

    if ( index1 == ( uint32_t)-1 )
        index1 = plot.m_plotdata.size();

    points.reserve( index1 - index0 + 10 );

    uint32_t idx0 = gi.prinfo_cur->plocs->front();
    ImU32 color_line = m_trace_events.m_events[ idx0 ].color;
    ImU32 color_point = imgui_col_complement( color_line );

    for ( size_t idx = index0; idx < plot.m_plotdata.size(); idx++ )
    {
        GraphPlot::plotdata_t &data = plot.m_plotdata[ idx ];
        float x = gi.ts_to_screenx( data.ts );
        float y = data.valf;

        if ( x <= 0.0f )
        {
            minval = y;
            maxval = y;
        }

        points.push_back( ImVec2( x, y ) );

        minval = std::min< float >( minval, y );
        maxval = std::max< float >( maxval, y );

        // Check if we're mouse hovering this event
        if ( gi.mouse_over )
            gi.add_mouse_hovered_event( x, get_event( data.eventid ) );

        if ( x >= gi.x + gi.w )
            break;
    }

    if ( points.size() )
    {
        bool closed = false;
        float thickness = 2.0f;
        bool anti_aliased = true;

        gi.prinfo_cur->minval = minval;
        gi.prinfo_cur->maxval = maxval;

        float pad = 0.15f * ( maxval - minval );
        if ( !pad )
            pad = 1.0f;
        minval -= pad;
        maxval += pad;

        float rcpdenom = gi.h / ( maxval - minval );
        for ( ImVec2 &pt : points )
            pt.y = gi.y + ( maxval - pt.y ) * rcpdenom;

        ImGui::GetWindowDrawList()->AddPolyline( points.data(), points.size(),
                                                 color_line, closed, thickness, anti_aliased );

        for ( const ImVec2 &pt : points )
        {
            imgui_drawrect( pt.x - imgui_scale( 1.5f ), imgui_scale( 3.0f ),
                            pt.y - imgui_scale( 1.5f ), imgui_scale( 3.0f ),
                            color_point );
        }
    }

    return points.size();
}

uint32_t TraceWin::graph_render_print_timeline( graph_info_t &gi )
{
    imgui_push_smallfont();

    struct row_draw_info_t
    {
        float x = 0.0f;
        const trace_event_t *event = nullptr;
        const TraceEvents::event_print_info_t *print_info = nullptr;
    };
    std::vector< row_draw_info_t > row_draw_info;

    uint32_t num_events = 0;
    const std::vector< uint32_t > &locs = *gi.prinfo_cur->plocs;
    bool timeline_labels = s_opts().getb( OPT_PrintTimelineLabels ) && !ImGui::GetIO().KeyAlt;

    uint32_t row_count = std::max< uint32_t >( 1, gi.h / gi.text_h - 1 );

    row_draw_info.resize( row_count + 1 );

    if ( m_trace_events.m_rect_size_max_x == -1.0f )
    {
        m_trace_events.update_ftraceprint_colors(
                    s_clrs().getalpha( col_Graph_PrintLabelSat ),
                    s_clrs().getalpha( col_Graph_PrintLabelAlpha ) );
    }

    // We need to start drawing to the left of 0 for timeline_labels
    int64_t ts = timeline_labels ? gi.screenx_to_ts( gi.x - m_trace_events.m_rect_size_max_x ) : gi.ts0;
    uint32_t eventstart = ts_to_eventid( ts );

    static float dx = imgui_scale( 3.0f );

    for ( size_t idx = vec_find_eventid( locs, eventstart );
          idx < locs.size();
          idx++ )
    {
        uint32_t eventid = locs[ idx ];
        const trace_event_t &event = get_event( eventid );
        uint32_t row_id = event.graph_row_id ? ( event.graph_row_id % row_count + 1 ) : 0;
        float x = gi.ts_to_screenx( event.ts );
        float y = gi.y + row_id * gi.text_h;

        if ( eventid > gi.eventend )
            break;
        else if ( gi.graph_only_filtered && event.is_filtered_out )
            continue;

        // Check if we drew something on this row already
        if ( row_draw_info[ row_id ].print_info )
        {
            const row_draw_info_t &draw_info = row_draw_info[ row_id ];
            float x0 = draw_info.x + dx;
            const TraceEvents::event_print_info_t *print_info = draw_info.print_info;

            // If we did and there is room, draw the ftrace print buf
            if ( x - x0 > print_info->rect_size.x )
                imgui_draw_text( x0, y + imgui_scale( 2.0f ), print_info->buf, draw_info.event->color );
        }

        // Otherwise draw a little tick for it
        imgui_drawrect( x, imgui_scale( 2.0f ), y, gi.text_h, event.color );

        // Check if we're mouse hovering this event
        if ( gi.mouse_over && gi.mouse_pos.y >= y && gi.mouse_pos.y <= y + gi.text_h )
            gi.add_mouse_hovered_event( x, event );

        num_events++;

        if ( timeline_labels )
        {
            row_draw_info[ row_id ].x = x;
            row_draw_info[ row_id ].print_info = m_trace_events.m_print_buf_info.get_val( event.id );
            row_draw_info[ row_id ].event = &event;
        }
    }

    for ( uint32_t row_id = 0; row_id < row_draw_info.size(); row_id++ )
    {
        const row_draw_info_t &draw_info = row_draw_info[ row_id ];
        const TraceEvents::event_print_info_t *print_info = draw_info.print_info;

        if ( print_info )
        {
            float x0 = draw_info.x + dx;
            float y = gi.y + row_id * gi.text_h;
            const trace_event_t *event = draw_info.event;

            imgui_draw_text( x0, y + imgui_scale( 2.0f ), print_info->buf, event->color );
        }
    }

    imgui_pop_smallfont();

    return num_events;
}

uint32_t TraceWin::graph_render_hw_row_timeline( graph_info_t &gi )
{
    imgui_push_smallfont();

    float row_h = gi.h;
    uint32_t num_events = 0;
    ImU32 col_event = s_clrs().get( col_Graph_1Event );

    ImRect hov_rect;
    ImU32 last_color = 0;
    float y = gi.y;
    bool draw_label = !ImGui::GetIO().KeyAlt;
    const std::vector< uint32_t > &locs = *gi.prinfo_cur->plocs;

    for ( size_t idx = vec_find_eventid( locs, gi.eventstart );
          idx < locs.size();
          idx++ )
    {
        const trace_event_t &fence_signaled = get_event( locs.at( idx ) );

        if ( fence_signaled.is_fence_signaled() &&
             is_valid_id( fence_signaled.id_start ) &&
             ( fence_signaled.ts - fence_signaled.duration < gi.ts1 ) )
        {
            float x0 = gi.ts_to_screenx( fence_signaled.ts - fence_signaled.duration );
            float x1 = gi.ts_to_screenx( fence_signaled.ts );

            imgui_drawrect( x0, x1 - x0, y, row_h, fence_signaled.color );

            // Draw a label if we have room.
            if ( draw_label )
            {
                const char *label = fence_signaled.user_comm;
                ImVec2 size = ImGui::CalcTextSize( label );

                if ( size.x + imgui_scale( 4 ) >= x1 - x0 )
                {
                    // No room for the comm, try just the pid.
                    label = strrchr( label, '-' );
                    if ( label )
                        size = ImGui::CalcTextSize( ++label );
                }
                if ( size.x + imgui_scale( 4 ) < x1 - x0 )
                {
                    ImGui::GetWindowDrawList()->AddText(
                                ImVec2( x0 + imgui_scale( 2.0f ), y + imgui_scale( 2.0f ) ),
                                s_clrs().get( col_Graph_BarText ), label );
                }
            }

            // If we drew the same color last time, draw a separator.
            if ( last_color == fence_signaled.color )
                imgui_drawrect( x0, 1.0, y, row_h, col_event );
            else
                last_color = fence_signaled.color;

            // Check if this fence_signaled is selected / hovered
            if ( ( gi.hovered_fence_signaled == fence_signaled.id ) ||
                 gi.mouse_pos_in_rect( x0, x1 - x0, y, row_h ) )
            {
                hov_rect = { x0, y, x1, y + row_h };

                if ( !is_valid_id( gi.hovered_fence_signaled ) )
                    gi.hovered_fence_signaled = fence_signaled.id;
            }

            num_events++;
        }
    }

    if ( hov_rect.Min.x < gi.x + gi.w )
        ImGui::GetWindowDrawList()->AddRect( hov_rect.Min, hov_rect.Max, s_clrs().get( col_Graph_BarSelRect ) );

    imgui_pop_smallfont();

    return num_events;
}

uint32_t TraceWin::graph_render_row_timeline( graph_info_t &gi )
{
    imgui_push_smallfont();

    ImRect hov_rect;
    uint32_t num_events = 0;
    ImU32 col_hwrunning = s_clrs().get( col_Graph_BarHwRunning );
    ImU32 col_userspace = s_clrs().get( col_Graph_BarUserspace );
    ImU32 col_hwqueue = s_clrs().get( col_Graph_BarHwQueue );
    ImU32 color_1event = s_clrs().get( col_Graph_1Event );
    const std::vector< uint32_t > &locs = *gi.prinfo_cur->plocs;

    uint32_t timeline_row_count = gi.h / gi.text_h;

    bool render_timeline_events = s_opts().getb( OPT_TimelineEvents );
    bool render_timeline_labels = s_opts().getb( OPT_TimelineLabels ) && !ImGui::GetIO().KeyAlt;

    for ( size_t idx = vec_find_eventid( locs, gi.eventstart );
          idx < locs.size();
          idx++ )
    {
        const trace_event_t &fence_signaled = get_event( locs[ idx ] );

        if ( fence_signaled.is_fence_signaled() &&
             is_valid_id( fence_signaled.id_start ) )
        {
            const trace_event_t &sched_run_job = get_event( fence_signaled.id_start );
            const trace_event_t &cs_ioctl = is_valid_id( sched_run_job.id_start ) ?
                        get_event( sched_run_job.id_start ) : sched_run_job;

            //$ TODO mikesart: can we bail out of this loop at some point if
            //  our start times for all the graphs are > gi.ts1?
            if ( cs_ioctl.ts < gi.ts1 )
            {
                bool hovered = false;
                float y = gi.y + ( fence_signaled.graph_row_id % timeline_row_count ) * gi.text_h;

                // amdgpu_cs_ioctl  amdgpu_sched_run_job   |   fence_signaled
                //       |-----------------|---------------|--------|
                //       |user-->          |hwqueue-->     |hw->    |
                float x_user_start = gi.ts_to_screenx( cs_ioctl.ts );
                float x_hwqueue_start = gi.ts_to_screenx( sched_run_job.ts );
                float x_hwqueue_end = gi.ts_to_screenx( fence_signaled.ts - fence_signaled.duration );
                float x_hw_end = gi.ts_to_screenx( fence_signaled.ts );
                float xleft = gi.timeline_render_user ? x_user_start : x_hwqueue_start;

                // Check if this fence_signaled is selected / hovered
                if ( ( gi.hovered_fence_signaled == fence_signaled.id ) ||
                    gi.mouse_pos_in_rect( xleft, x_hw_end - xleft, y, gi.text_h ) )
                {
                    // Mouse is hovering over this fence_signaled.
                    hovered = true;
                    hov_rect = { x_user_start, y, x_hw_end, y + gi.text_h };

                    if ( !is_valid_id( gi.hovered_fence_signaled ) )
                        gi.hovered_fence_signaled = fence_signaled.id;
                }

                // Draw user bar
                if ( hovered || gi.timeline_render_user )
                    imgui_drawrect( x_user_start, x_hwqueue_start - x_user_start, y, gi.text_h, col_userspace );

                // Draw hw queue bar
                if ( x_hwqueue_end != x_hwqueue_start )
                    imgui_drawrect( x_hwqueue_start, x_hwqueue_end - x_hwqueue_start, y, gi.text_h, col_hwqueue );

                // Draw hw running bar
                imgui_drawrect( x_hwqueue_end, x_hw_end - x_hwqueue_end, y, gi.text_h, col_hwrunning );

                if ( render_timeline_labels )
                {
                    const ImVec2 size = ImGui::CalcTextSize( cs_ioctl.user_comm );
                    float x_text = std::max< float >( x_hwqueue_start, gi.x ) + imgui_scale( 2.0f );

                    if ( x_hw_end - x_text >= size.x )
                    {
                        ImGui::GetWindowDrawList()->AddText( ImVec2( x_text, y + imgui_scale( 1.0f ) ),
                                                             s_clrs().get( col_Graph_BarText ), cs_ioctl.user_comm );
                    }
                }

                if ( render_timeline_events )
                {
                    if ( cs_ioctl.id != sched_run_job.id )
                    {
                        // Draw event line for start of user
                        imgui_drawrect( x_user_start, 1.0, y, gi.text_h, color_1event );

                        // Check if we're mouse hovering starting event
                        if ( gi.mouse_over && gi.mouse_pos.y >= y && gi.mouse_pos.y <= y + gi.text_h )
                        {
                            // If we are hovering, and no selection bar is set, do it.
                            if ( gi.add_mouse_hovered_event( x_user_start, cs_ioctl ) && ( hov_rect.Min.x == FLT_MAX ) )
                            {
                                hov_rect = { x_user_start, y, x_hw_end, y + gi.text_h };

                                // Draw user bar for hovered events if they weren't already drawn
                                if ( !hovered && !gi.timeline_render_user )
                                    imgui_drawrect( x_user_start, x_hwqueue_start - x_user_start, y, gi.text_h, col_userspace );
                            }
                        }
                    }

                    // Draw event line for hwqueue start and hw end
                    imgui_drawrect( x_hwqueue_start, 1.0, y, gi.text_h, color_1event );
                    imgui_drawrect( x_hw_end, 1.0, y, gi.text_h, color_1event );
                }

                num_events++;
            }
        }
    }

    if ( hov_rect.Min.x < gi.x + gi.w )
        ImGui::GetWindowDrawList()->AddRect( hov_rect.Min, hov_rect.Max, s_clrs().get( col_Graph_BarSelRect ) );

    imgui_pop_smallfont();

    return num_events;
}

uint32_t TraceWin::graph_render_row_events( graph_info_t &gi )
{
    uint32_t num_events = 0;
    bool draw_hovered_event = false;
    bool draw_selected_event = false;
    const std::vector< uint32_t > &locs = *gi.prinfo_cur->plocs;
    event_renderer_t event_renderer( gi.y + 4, gi.w, gi.h - 8 );

    for ( size_t idx = vec_find_eventid( locs, gi.eventstart );
          idx < locs.size();
          idx++ )
    {
        uint32_t eventid = locs[ idx ];
        const trace_event_t &event = get_event( eventid );

        if ( eventid > gi.eventend )
            break;
        else if ( gi.graph_only_filtered && event.is_filtered_out )
            continue;

        float x = gi.ts_to_screenx( event.ts );

        if ( eventid == m_eventlist.hovered_eventid )
            draw_hovered_event = true;
        else if ( eventid == m_eventlist.selected_eventid )
            draw_selected_event = true;

        // Check if we're mouse hovering this event
        if ( gi.mouse_over )
            gi.add_mouse_hovered_event( x, event );

        event_renderer.add_event( x );
        num_events++;
    }

    event_renderer.done();

    if ( draw_hovered_event )
    {
        trace_event_t &event = get_event( m_eventlist.hovered_eventid );
        float x = gi.ts_to_screenx( event.ts );

        ImGui::GetWindowDrawList()->AddCircleFilled(
                    ImVec2( x, gi.y + gi.h / 2.0f ),
                    imgui_scale( 5.0f ),
                    s_clrs().get( col_Graph_HovEvent ) );
    }

    if ( draw_selected_event )
    {
        trace_event_t &event = get_event( m_eventlist.selected_eventid );
        float x = gi.ts_to_screenx( event.ts );

        ImGui::GetWindowDrawList()->AddCircleFilled(
                    ImVec2( x, gi.y + gi.h / 2.0f ),
                    imgui_scale( 5.0f ),
                    s_clrs().get( col_Graph_SelEvent ) );
    }

    return num_events;
}

void TraceWin::graph_render_row( graph_info_t &gi )
{
    if ( gi.mouse_over )
    {
        m_graph.mouse_over_row_name = gi.prinfo_cur->row_name;
        m_graph.mouse_over_row_type = gi.prinfo_cur->row_type;
    }

    // Draw background
    ImGui::GetWindowDrawList()->AddRectFilled(
        ImVec2( gi.x, gi.y ),
        ImVec2( gi.x + gi.w, gi.y + gi.h ),
        s_clrs().get( col_Graph_RowBk ) );

    // Call the render callback function
    gi.prinfo_cur->num_events = gi.prinfo_cur->render_cb ? gi.prinfo_cur->render_cb( gi ) : 0;
}

void TraceWin::graph_render_time_ticks( class graph_info_t &gi )
{
    // Draw time ticks every millisecond
    int64_t tsstart = std::max< int64_t >( gi.ts0 / NSECS_PER_MSEC - 1, 0 ) * NSECS_PER_MSEC;
    float dx = gi.w * NSECS_PER_MSEC * gi.tsdxrcp;

    if ( dx <= imgui_scale( 4.0f ) )
    {
        tsstart = std::max< int64_t >( gi.ts0 / NSECS_PER_SEC - 1, 0 ) * NSECS_PER_SEC;
        dx = gi.w * NSECS_PER_SEC * gi.tsdxrcp;
    }

    if ( dx > imgui_scale( 4.0f ) )
    {
        float x0 = gi.ts_to_x( tsstart );

        for ( ; x0 <= gi.w; x0 += dx )
        {
            imgui_drawrect( gi.x + x0, imgui_scale( 1.0f ),
                            gi.y, imgui_scale( 16.0f ),
                            s_clrs().get( col_Graph_TimeTick ) );

            if ( dx >= imgui_scale( 35.0f ) )
            {
                for ( int i = 1; i < 4; i++ )
                {
                    imgui_drawrect( gi.x + x0 + i * dx / 4, imgui_scale( 1.0f ),
                                    gi.y, imgui_scale( 4.0f ),
                                    s_clrs().get( col_Graph_TimeTick ) );
                }
            }
        }
    }
}

static float get_vblank_xdiffs( TraceWin *win, graph_info_t &gi, const std::vector< uint32_t > *vblank_locs )
{
    float xdiff = 0.0f;
    float xlast = 0.0f;
    uint32_t count = 0;

    for ( size_t idx = vec_find_eventid( *vblank_locs, gi.eventstart );
          idx < vblank_locs->size();
          idx++ )
    {
        uint32_t id = vblank_locs->at( idx );
        trace_event_t &event = win->get_event( id );

        if ( s_opts().getcrtc( event.crtc ) )
        {
            float x = gi.ts_to_screenx( event.ts );

            if ( xlast )
                xdiff = std::max< float >( xdiff, x - xlast );
            xlast = x;

            if ( count++ >= 10 )
                break;
        }
    }

    return xdiff;
}

void TraceWin::graph_render_vblanks( graph_info_t &gi )
{
    // Draw vblank events on every graph.
    const std::vector< uint32_t > *vblank_locs = m_trace_events.get_tdopexpr_locs( "$name=drm_vblank_event" );

    if ( vblank_locs )
    {
        /*
         * From Pierre-Loup: One thing I notice when zooming out is that things become
         * very noisy because of the vblank bars. I'm changing their colors so they're not
         * fullbright, which helps, but can they be changed to be in the background of
         * other rendering past a certain zoom threshold? You want them in the foreground
         * when pretty close, but in the background if there's more than ~50 on screen
         * probably?
         */
        float xdiff = get_vblank_xdiffs( this, gi, vblank_locs ) / imgui_scale( 1.0f );
        uint32_t alpha = std::min< uint32_t >( 255, 50 + 2 * xdiff );

        for ( size_t idx = vec_find_eventid( *vblank_locs, gi.eventstart );
              idx < vblank_locs->size();
              idx++ )
        {
            uint32_t id = vblank_locs->at( idx );

            if ( id > gi.eventend )
                break;

            trace_event_t &event = get_event( id );

            if ( s_opts().getcrtc( event.crtc ) )
            {
                // drm_vblank_event0: blue, drm_vblank_event1: red
                colors_t col = ( event.crtc > 0 ) ? col_VBlank1 : col_VBlank0;
                float x = gi.ts_to_screenx( event.ts );

                imgui_drawrect( x, imgui_scale( 1.0f ),
                                gi.y, gi.h,
                                s_clrs().get( col, alpha ) );
            }
        }
    }
}

void TraceWin::graph_render_mouse_pos( graph_info_t &gi )
{
    // Draw location line for mouse if mouse is over graph
    if ( m_graph.is_mouse_over &&
         gi.mouse_pos.x >= gi.x &&
         gi.mouse_pos.x <= gi.x + gi.w )
    {
        imgui_drawrect( gi.mouse_pos.x, imgui_scale( 2.0f ),
                        gi.y, gi.h,
                        s_clrs().get( col_Graph_MousePos ) );
    }

    // Render markers A/B if in range
    for ( size_t i = 0; i < ARRAY_SIZE( m_graph.ts_markers ); i++ )
    {
        if ( ( m_graph.ts_markers[ i ] >= gi.ts0 ) && ( m_graph.ts_markers[ i ] < gi.ts1 ) )
        {
            float x = gi.ts_to_screenx( m_graph.ts_markers[ i ] );

            imgui_drawrect( x, imgui_scale( 2.0f ),
                            gi.y, gi.h, s_clrs().get( col_Graph_MarkerA + i ) );
        }
    }
}

void TraceWin::graph_render_eventids( class graph_info_t &gi )
{
    if ( is_valid_id( m_eventlist.hovered_eventid ) )
    {
        trace_event_t &event = get_event( m_eventlist.hovered_eventid );

        if ( event.ts >= gi.ts0 && event.ts <= gi.ts1 )
        {
            float x = gi.ts_to_screenx( event.ts );

            imgui_drawrect( x, imgui_scale( 1.0f ),
                            gi.y, gi.h,
                            s_clrs().get( col_Graph_HovEvent, 120 ) );
        }
    }

    if ( is_valid_id( m_eventlist.selected_eventid ) )
    {
        trace_event_t &event = get_event( m_eventlist.selected_eventid );

        if ( event.ts >= gi.ts0 && event.ts <= gi.ts1 )
        {
            float x = gi.ts_to_screenx( event.ts );

            imgui_drawrect( x, imgui_scale( 1.0f ),
                            gi.y, gi.h,
                            s_clrs().get( col_Graph_SelEvent, 120 ) );
        }
    }
}

void TraceWin::graph_render_mouse_selection( class graph_info_t &gi )
{
    // Draw mouse selection location
    if ( ( m_graph.mouse_captured == MOUSE_CAPTURED_ZOOM ) ||
         ( m_graph.mouse_captured == MOUSE_CAPTURED_SELECT_AREA ) )
    {
        float mousex0 = m_graph.mouse_capture_pos.x;
        float mousex1 = gi.mouse_pos.x;

        imgui_drawrect( mousex0, mousex1 - mousex0,
                        gi.y, gi.h,
                        s_clrs().get( col_Graph_ZoomSel ) );
    }
}

void TraceWin::graph_render_eventlist_selection( class graph_info_t &gi )
{
    if ( s_opts().getb( OPT_ShowEventList ) )
    {
        // Draw rectangle for visible event list contents
        if ( is_valid_id( m_eventlist.start_eventid ) &&
             is_valid_id( m_eventlist.end_eventid ) )
        {
            trace_event_t &event0 = get_event( m_eventlist.start_eventid );
            trace_event_t &event1 = get_event( m_eventlist.end_eventid - 1 );
            float xstart = gi.ts_to_screenx( event0.ts );
            float xend = gi.ts_to_screenx( event1.ts );

            ImGui::GetWindowDrawList()->AddRect(
                        ImVec2( xstart, gi.y + imgui_scale( 20 ) ),
                        ImVec2( xend, gi.y + gi.h - imgui_scale( 30 ) ),
                        s_clrs().get( col_EventList_Sel ) );
        }
    }
}

static void render_row_label( float x, float y, row_info_t &ri )
{
    std::string label = string_format( "%u) %s", ri.id, ri.row_name.c_str() );
    imgui_draw_text( x, y, label.c_str(), s_clrs().get( col_Graph_RowLabelText ), true );
    y += ImGui::GetTextLineHeight();

    if ( ri.minval <= ri.maxval )
    {
        label = string_format( "min:%.2f max:%.2f", ri.minval, ri.maxval );
        imgui_draw_text( x, y, label.c_str(), s_clrs().get( col_Graph_RowLabelText ), true );
    }
    else if ( ri.num_events )
    {
        label = string_format( "%u events", ri.num_events );
        imgui_draw_text( x, y, label.c_str(), s_clrs().get( col_Graph_RowLabelText ), true );
    }
}

void TraceWin::graph_render_row_labels( graph_info_t &gi )
{
    if ( gi.prinfo_zoom )
    {
        if ( gi.prinfo_zoom_hw )
        {
            float y = gi.y + gi.h - gi.prinfo_zoom_hw->row_h;

            render_row_label( gi.x, y, *gi.prinfo_zoom_hw );
        }

        render_row_label( gi.x, gi.y, *gi.prinfo_zoom );
    }
    else
    {
        for ( row_info_t &ri : gi.row_info )
        {
            float y = gi.y + ri.row_y;

            render_row_label( gi.x, y, ri );
        }
    }
}

void TraceWin::graph_range_check_times()
{
    const std::vector< trace_event_t > &events = m_trace_events.m_events;

    if ( m_graph.length_ts < m_graph.s_min_length )
    {
        m_graph.length_ts = m_graph.s_min_length;
        m_graph.recalc_timebufs = true;
    }
    else if ( m_graph.length_ts > m_graph.s_max_length )
    {
        m_graph.length_ts = m_graph.s_max_length;
        m_graph.recalc_timebufs = true;
    }

    // Sanity check the graph start doesn't go completely off the rails.
    if ( m_graph.start_ts + m_eventlist.tsoffset < events.front().ts - 1 * NSECS_PER_MSEC )
    {
        m_graph.start_ts = events.front().ts - m_eventlist.tsoffset - 1 * NSECS_PER_MSEC;
        m_graph.recalc_timebufs = true;
    }
    else if ( m_graph.start_ts + m_eventlist.tsoffset > events.back().ts )
    {
        m_graph.start_ts = events.back().ts - m_eventlist.tsoffset;
        m_graph.recalc_timebufs = true;
    }
}

void TraceWin::graph_zoom( int64_t center_ts, int64_t ts0, bool zoomin, int64_t newlenin )
{
    int64_t origlen = m_graph.length_ts;
    int64_t amt = zoomin ? -( origlen / 2 ) : ( origlen / 2 );
    int64_t newlen = ( newlenin != INT64_MAX ) ? newlenin :
            Clamp< int64_t >( origlen + amt, m_graph.s_min_length, m_graph.s_max_length );

    if ( newlen != origlen )
    {
        double scale = ( double )newlen / origlen;

        m_graph.start_ts = center_ts - ( int64_t )( ( center_ts - ts0 ) * scale ) - m_eventlist.tsoffset;
        m_graph.length_ts = newlen;
        m_graph.recalc_timebufs = true;
    }
}

bool TraceWin::is_graph_row_zoomable()
{
    if ( !m_graph.mouse_over_row_name.empty() )
    {
        if ( m_graph.zoom_row_name != m_graph.mouse_over_row_name )
        {
            if ( m_graph.mouse_over_row_type == TraceEvents::LOC_TYPE_Timeline ||
                 m_graph.mouse_over_row_type == TraceEvents::LOC_TYPE_Timeline_hw ||
                 m_graph.mouse_over_row_type == TraceEvents::LOC_TYPE_Plot ||
                 m_graph.mouse_over_row_type == TraceEvents::LOC_TYPE_Print )
            {
                return true;
            }
        }
    }

    return false;
}

void TraceWin::zoom_graph_row()
{
    m_graph.zoom_row_name = m_graph.mouse_over_row_name;

    if ( m_graph.mouse_over_row_type == TraceEvents::LOC_TYPE_Timeline_hw )
    {
        // Trim " hw" from end of string so, for example, we zoom "gfx" and not "gfx hw".
        m_graph.zoom_row_name.resize( m_graph.zoom_row_name.size() - 3 );
    }
}

void TraceWin::graph_handle_hotkeys( graph_info_t &gi )
{
    if ( m_graph.saved_locs.size() < 9 )
        m_graph.saved_locs.resize( 9 );

    if ( ImGui::GetIO().KeyCtrl )
    {
        bool keyshift = ImGui::GetIO().KeyShift;

        if ( keyshift && ImGui::IsKeyPressed( 'z' ) )
        {
            if ( !m_graph.zoom_row_name.empty() )
                m_graph.zoom_row_name.clear();
            else if ( is_graph_row_zoomable() )
                zoom_graph_row();
        }
        else  if ( ImGui::IsKeyPressed( 'a' ) || ImGui::IsKeyPressed( 'b' ) )
        {
            int index = ImGui::IsKeyPressed( 'a' ) ? 0 : 1;

            if ( keyshift )
            {
                graph_marker_set( index, m_graph.ts_marker_mouse );
            }
            else if ( graph_marker_valid( index ) )
            {
                m_graph.start_ts = m_graph.ts_markers[ index ] - m_graph.length_ts / 2;
                m_graph.recalc_timebufs = true;
            }
        }
        else
        {
            for ( int key = '1'; key <= '9'; key++ )
            {
                if ( ImGui::IsKeyPressed( key ) )
                {
                    int index = key - '1';

                    if ( keyshift )
                    {
                        // ctrl+shift+#: save location
                        m_graph.saved_locs[ index ] = std::make_pair( m_graph.start_ts, m_graph.length_ts );
                    }
                    else if ( m_graph.saved_locs[ index ].second )
                    {
                        // ctrl+#: goto location
                        m_graph.start_ts = m_graph.saved_locs[ index ].first;
                        m_graph.length_ts = m_graph.saved_locs[ index ].second;
                        m_graph.recalc_timebufs = true;
                    }
                    break;
                }
            }
        }
    }
    else if ( ImGui::IsWindowFocused() && ImGui::IsKeyPressed( 'z' ) )
    {
        if ( m_graph.zoom_loc.first != INT64_MAX )
        {
            m_graph.start_ts = m_graph.zoom_loc.first;
            m_graph.length_ts = m_graph.zoom_loc.second;
            m_graph.recalc_timebufs = true;

            m_graph.zoom_loc = std::make_pair( INT64_MAX, INT64_MAX );
        }
        else
        {
            int64_t newlen = 3 * NSECS_PER_MSEC;
            int64_t mouse_ts = gi.screenx_to_ts( gi.mouse_pos.x );

            m_graph.zoom_loc = std::make_pair( m_graph.start_ts, m_graph.length_ts );

            graph_zoom( mouse_ts, gi.ts0, false, newlen );
        }
    }
}

void TraceWin::graph_handle_keyboard_scroll()
{
    if ( !ImGui::IsWindowFocused() )
        return;

    int64_t start_ts = m_graph.start_ts + m_eventlist.tsoffset;
    const std::vector< trace_event_t > &events = m_trace_events.m_events;

    if ( imgui_key_pressed( ImGuiKey_UpArrow ) )
    {
        m_graph.start_y += ImGui::GetTextLineHeightWithSpacing() * 4;
    }
    else if ( imgui_key_pressed( ImGuiKey_DownArrow ) )
    {
        m_graph.start_y -= ImGui::GetTextLineHeightWithSpacing() * 4;
    }
    else if ( imgui_key_pressed( ImGuiKey_LeftArrow ) )
    {
        start_ts = std::max< int64_t >( start_ts - 9 * m_graph.length_ts / 10,
                                        -NSECS_PER_MSEC );
    }
    else if ( imgui_key_pressed( ImGuiKey_RightArrow ) )
    {
        start_ts = std::min< int64_t >( start_ts + 9 * m_graph.length_ts / 10,
                                        events.back().ts - m_graph.length_ts + NSECS_PER_MSEC );
    }
    else if ( imgui_key_pressed( ImGuiKey_Home ) )
    {
        start_ts = events.front().ts - NSECS_PER_MSEC;
    }
    else if ( imgui_key_pressed( ImGuiKey_End ) )
    {
        start_ts = events.back().ts - m_graph.length_ts + NSECS_PER_MSEC;
    }

    start_ts -= m_eventlist.tsoffset;
    if ( start_ts != m_graph.start_ts )
    {
        m_graph.start_ts = start_ts;
        m_graph.recalc_timebufs = true;
    }
}

static void calc_process_graph_height( TraceWin *win, graph_info_t &gi )
{
    // Zoom mode if we have a gfx row and zoom option is set
    option_id_t optid;
    float max_graph_size;

    if ( gi.prinfo_zoom )
    {
        optid = OPT_GraphHeightZoomed;
        max_graph_size = imgui_scale( 60.0f ) * gi.row_h;
    }
    else
    {
        optid = OPT_GraphHeight;
        max_graph_size = gi.total_graph_height;
    }

    // Set up min / max sizes and clamp value in that range
    float valf = s_opts().getf( optid );
    float valf_min = 4.0f * gi.row_h;
    float valf_max = Clamp< float >( max_graph_size, valf_min, ImGui::GetWindowHeight() );

    // First time initialization - start with about 15 rows
    if ( !valf )
        valf = 15.0f * gi.row_h;

    valf = Clamp< float >( valf, valf_min, valf_max );
    s_opts().setf( optid, valf, valf_min, valf_max );

    gi.visible_graph_height = valf;
}

void TraceWin::graph_render()
{
    graph_info_t gi;

    // Initialize our row size, location, etc information based on our graph rows
    gi.init_row_info( this, m_graph.rows.m_graph_rows_list );

    if ( !m_graph.zoom_row_name.empty() )
    {
        gi.prinfo_zoom = gi.find_row( m_graph.zoom_row_name.c_str() );
        if ( gi.prinfo_zoom )
            gi.prinfo_zoom_hw = gi.find_row( ( m_graph.zoom_row_name + " hw" ).c_str() );
    }

    if ( gi.prinfo_zoom )
    {
        ImGui::SameLine();

        std::string label = string_format( "Unzoom '%s'", m_graph.zoom_row_name.c_str() );
        if ( ImGui::Button( label.c_str() ) )
            m_graph.zoom_row_name.clear();
    }

    // Figure out gi.visible_graph_height
    calc_process_graph_height( this, gi );

    // Make sure ts start and length values are mostly sane
    graph_range_check_times();

    ImGui::BeginChild( "EventGraph", ImVec2( 0, gi.visible_graph_height ), true );
    {
        ImVec2 windowpos = ImVec2( ImGui::GetWindowClipRectMin().x, ImGui::GetWindowPos().y );
        ImVec2 windowsize = ImGui::GetWindowSize();

        // Clear graph background
        imgui_drawrect( windowpos.x, windowsize.x,
                        windowpos.y, windowsize.y, s_clrs().get( col_Graph_Bk ) );

        // Initialize our graphics info struct
        gi.init( this, windowpos.x, windowsize.x );

        // Range check mouse pan values
        m_graph.start_y = Clamp< float >( m_graph.start_y,
                                          gi.visible_graph_height - gi.total_graph_height, 0.0f );

        // If we don't have a popup menu, clear the mouse over row name
        if ( !m_graph.popupmenu )
        {
            m_graph.mouse_over_row_name = "";
            m_graph.mouse_over_row_type = TraceEvents::LOC_TYPE_Max;
            m_graph.rename_comm_buf[ 0 ] = 0;
        }

        // If we have a gfx graph and we're zoomed, render only that
        float start_y = gi.prinfo_zoom ? 0 : m_graph.start_y;
        if ( gi.prinfo_zoom )
        {
            float gfx_hw_row_h = 0;

            if ( gi.prinfo_zoom_hw )
            {
                row_info_t &ri = *gi.prinfo_zoom_hw;
                gfx_hw_row_h = ri.row_h + ImGui::GetStyle().FramePadding.y;

                gi.set_pos_y( windowpos.y + windowsize.y - ri.row_h, ri.row_h, &ri );
                graph_render_row( gi );
            }

            gi.timeline_render_user = true;
            gi.set_pos_y( windowpos.y, windowsize.y - gfx_hw_row_h, gi.prinfo_zoom );
            graph_render_row( gi );
        }
        else
        {
            // Pass 0: Render all !timeline rows
            // Pass 1: Render all timeline rows
            for ( int pass = 0; pass < 2; pass++ )
            {
                bool render_timelines = !!pass;

                for ( row_info_t &ri : gi.row_info )
                {
                    bool is_timeline = ( ri.row_type == TraceEvents::LOC_TYPE_Timeline );

                    if ( is_timeline == render_timelines )
                    {
                        gi.set_pos_y( windowpos.y + ri.row_y + start_y, ri.row_h, &ri );
                        graph_render_row( gi );
                    }
                }
            }
        }

        // Render full graph ticks, vblanks, cursor pos, etc.
        gi.set_pos_y( windowpos.y, windowsize.y, NULL );
        graph_render_time_ticks( gi );
        graph_render_vblanks( gi );
        graph_render_mouse_pos( gi );
        graph_render_eventids( gi );
        graph_render_mouse_selection( gi );
        graph_render_eventlist_selection( gi );

        // Render row labels last (taking panning into consideration)
        gi.set_pos_y( windowpos.y + start_y, windowsize.y, NULL );
        graph_render_row_labels( gi );

        // Handle right, left, pgup, pgdown, etc in graph
        graph_handle_keyboard_scroll();

        // Handle hotkeys. Ie: Ctrl+Shift+1, etc
        graph_handle_hotkeys( gi );

        // Render mouse tooltips, mouse selections, etc
        gi.set_pos_y( windowpos.y, windowsize.y, NULL );
        graph_handle_mouse( gi );
    }
    ImGui::EndChild();

    ImGui::Button( "##resize_graph", ImVec2( ImGui::GetContentRegionAvailWidth(), imgui_scale( 4.0f ) ) );
    if ( ImGui::IsItemHovered() )
        ImGui::SetMouseCursor( ImGuiMouseCursor_ResizeNS );
    if ( ImGui::IsItemActive() && imgui_mousepos_valid( gi.mouse_pos ) )
    {
        option_id_t opt = gi.prinfo_zoom ? OPT_GraphHeightZoomed : OPT_GraphHeight;

        if ( ImGui::IsMouseClicked( 0 ) )
            m_graph.resize_graph_click_pos = s_opts().getf( opt );

        s_opts().setf( opt, m_graph.resize_graph_click_pos + ImGui::GetMouseDragDelta( 0 ).y );
    }
}

bool TraceWin::graph_render_popupmenu( graph_info_t &gi )
{
    option_id_t optid = OPT_Invalid;

    if ( !ImGui::BeginPopup( "GraphPopup" ) )
        return false;

    auto get_location_label_lambda = [this]( size_t i )
    {
        auto &pair = m_graph.saved_locs[ i ];
        std::string start = ts_to_timestr( pair.first );
        std::string len = ts_to_timestr( pair.second );
        return string_format( "Start:%s Length:%s", start.c_str(), len.c_str() );
    };

    imgui_text_bg( "Options", ImGui::GetColorVec4( ImGuiCol_Header ) );
    ImGui::Separator();

    if ( !m_graph.zoom_row_name.empty() )
    {
        std::string label = string_format( "Unzoom row '%s'", m_graph.zoom_row_name.c_str() );

        if ( ImGui::MenuItem( label.c_str() ) )
            m_graph.zoom_row_name.clear();
    }

    if ( !m_graph.mouse_over_row_name.empty() )
    {
        std::string label;

        if ( is_graph_row_zoomable() )
        {
            label = string_format( "Zoom row '%s'", m_graph.mouse_over_row_name.c_str() );

            if ( ImGui::MenuItem( label.c_str() ) )
                zoom_graph_row();
        }

        optid = get_comm_option_id( m_loader, m_graph.mouse_over_row_name.c_str() );
        label = string_format( "Hide row '%s'", m_graph.mouse_over_row_name.c_str() );

        if ( ImGui::MenuItem( label.c_str() ) )
            m_graph.rows.show_row( m_graph.mouse_over_row_name, GraphRows::HIDE_ROW );

        label = string_format( "Hide row '%s' and below", m_graph.mouse_over_row_name.c_str() );
        if ( ImGui::MenuItem( label.c_str() ) )
            m_graph.rows.show_row( m_graph.mouse_over_row_name, GraphRows::HIDE_ROW_AND_ALL_BELOW );
    }

    if ( !m_graph.rows_hidden_rows.empty() )
    {
        if ( ImGui::BeginMenu( "Show row" ) )
        {
            if ( ImGui::MenuItem( "All Rows" ) )
                m_graph.rows.show_row( "", GraphRows::SHOW_ALL_ROWS );

            ImGui::Separator();

            for ( const GraphRows::graph_rows_info_t &entry : m_graph.rows_hidden_rows )
            {
                const std::string label = string_format( "%s (%lu events)",
                                                         entry.row_name.c_str(), entry.event_count );

                if ( ImGui::MenuItem( label.c_str() ) )
                {
                    m_graph.rows.show_row( entry.row_name.c_str(), GraphRows::SHOW_ROW );
                }
            }

            ImGui::EndMenu();
        }
    }

    if ( !m_graph.mouse_over_row_name.empty() )
    {
        std::string move_label = string_format( "Move '%s' after", m_graph.mouse_over_row_name.c_str() );

        if ( ImGui::BeginMenu( move_label.c_str() ) )
        {
            for ( const GraphRows::graph_rows_info_t &entry : m_graph.rows.m_graph_rows_list )
            {
                if ( !entry.hidden && ( entry.row_name != m_graph.mouse_over_row_name ) )
                {
                    if ( ImGui::MenuItem( entry.row_name.c_str() ) )
                    {
                        m_graph.rows.move_row( m_graph.mouse_over_row_name, entry.row_name );
                        ImGui::CloseCurrentPopup();
                        break;
                    }
                }
            }

            ImGui::EndMenu();
        }
    }

    {
        if ( imgui_input_text2( "New Graph Row:", m_graph.new_row_buf, 0, ImGuiInputTextFlags_EnterReturnsTrue ) )
        {
            m_graph.new_row_errstr.clear();

            if ( m_trace_events.get_tdopexpr_locs( m_graph.new_row_buf, &m_graph.new_row_errstr ) )
            {
                m_graph.rows.add_row( m_trace_events, m_graph.new_row_buf );
                ImGui::CloseCurrentPopup();
            }
            else if ( m_graph.new_row_errstr.empty() )
            {
                m_graph.new_row_errstr = string_format( "ERROR: no events found for '%s'", m_graph.new_row_buf );
            }
        }

        if ( ImGui::IsItemHovered() )
        {
            std::string tooltip;

            tooltip += s_textclrs().bright_str( "Add a new row with filtered events\n\n" );
            tooltip += "Examples:\n";
            tooltip += "  $pid = 4615\n";
            tooltip += "  $duration >= 5.5\n";
            tooltip += "  $buf =~ \"[Compositor] Warp\"\n";
            tooltip += "  ( $timeline = gfx ) && ( $id < 10 || $id > 100 )";

            ImGui::SetTooltip( "%s", tooltip.c_str() );
        }

        if ( !m_graph.new_row_errstr.empty() )
            ImGui::TextColored( ImVec4( 1, 0, 0, 1), "%s", m_graph.new_row_errstr.c_str() );
    }

    if ( is_valid_id( m_graph.hovered_eventid ) &&
         strncmp( m_graph.mouse_over_row_name.c_str(), "plot:", 5 ) )
    {
        const trace_event_t &event = m_trace_events.m_events[ m_graph.hovered_eventid ];
        const char *plot_str = CreatePlotDlg::get_plot_str( event );

        if ( plot_str )
        {
            std::string plot_label = std::string( "Create Plot for " ) + s_textclrs().bright_str( plot_str );

            if ( ImGui::MenuItem( plot_label.c_str() ) )
                m_create_plot_eventid = event.id;
        }
    }

    if ( m_trace_events.get_comm_locs( m_graph.mouse_over_row_name.c_str() ) )
    {
        if ( !m_graph.rename_comm_buf[ 0 ] )
        {
            strcpy_safe( m_graph.rename_comm_buf, m_graph.mouse_over_row_name.c_str() );

            char *slash = strrchr( m_graph.rename_comm_buf, '-' );
            if ( slash )
                *slash = 0;
        }

        std::string label = string_format( "Rename '%s':", m_graph.mouse_over_row_name.c_str() );
        if ( imgui_input_text2( label.c_str(), m_graph.rename_comm_buf, 0, ImGuiInputTextFlags_EnterReturnsTrue ) )
        {
            if ( rename_comm_event( m_graph.mouse_over_row_name.c_str(), m_graph.rename_comm_buf ) )
                ImGui::CloseCurrentPopup();
        }
    }

    if ( optid != OPT_Invalid )
        s_opts().render_imgui_opt( optid );

    ImGui::Separator();

    if ( ImGui::BeginMenu( "Set Marker" ) )
    {
        for ( size_t i = 0; i < ARRAY_SIZE( m_graph.ts_markers ); i++ )
        {
            ImGui::PushID( i );

            char label[ 2 ];
            std::string shortcut = string_format( "Ctrl+Shift+%c", char( 'A' + i ) );

            label[ 0 ] = char( 'A' + i );
            label[ 1 ] = 0;
            if ( ImGui::MenuItem( label, shortcut.c_str() ) )
                graph_marker_set( i, m_graph.ts_marker_mouse );

            ImGui::PopID();
        }

        ImGui::EndMenu();
    }

    if ( ( graph_marker_valid( 0 ) || graph_marker_valid( 1 ) )
         && ImGui::BeginMenu( "Clear Marker" ) )
    {
        for ( size_t i = 0; i < ARRAY_SIZE( m_graph.ts_markers ); i++ )
        {
            if ( !graph_marker_valid( i ) )
                continue;

            ImGui::PushID( i );

            char label[ 2 ];
            label[ 0 ] = char( 'A' + i );
            label[ 1 ] = 0;
            if ( ImGui::MenuItem( label ) )
                graph_marker_set( i, INT64_MAX );

            ImGui::PopID();
        }


        ImGui::EndMenu();
    }

    if ( ImGui::BeginMenu( "Save Location" ) )
    {
        for ( size_t i = 0; i < m_graph.saved_locs.size(); i++ )
        {
            std::string label = get_location_label_lambda( i );
            std::string shortcut = string_format( "Ctrl+Shift+%c", ( int )( i + '1' ) );

            if ( ImGui::MenuItem( label.c_str(), shortcut.c_str() ) )
            {
                m_graph.saved_locs[ i ] = std::make_pair( m_graph.start_ts, m_graph.length_ts );
                break;
            }
        }

        ImGui::EndMenu();
    }

    if ( ImGui::BeginMenu( "Restore Location" ) )
    {
        for ( size_t i = 0; i < m_graph.saved_locs.size(); i++ )
        {
            if ( m_graph.saved_locs[ i ].second )
            {
                std::string label = get_location_label_lambda( i );
                std::string shortcut = string_format( "Ctrl+%c", ( int )( i + '1' ) );

                if ( ImGui::MenuItem( label.c_str(), shortcut.c_str() ) )
                {
                    m_graph.start_ts = m_graph.saved_locs[ i ].first;
                    m_graph.length_ts = m_graph.saved_locs[ i ].second;
                    m_graph.recalc_timebufs = true;
                }
            }
        }

        ImGui::EndMenu();
    }

    ImGui::Separator();

    s_opts().render_imgui_options( m_loader.m_crtc_max );

    ImGui::EndPopup();
    return true;
}

void TraceWin::graph_handle_mouse_captured( graph_info_t &gi )
{
    // Uncapture mouse if user hits escape
    if ( m_graph.mouse_captured && imgui_key_pressed( ImGuiKey_Escape ) )
    {
        m_graph.mouse_captured = MOUSE_NOT_CAPTURED;
        ImGui::CaptureMouseFromApp( false );

        return;
    }

    bool is_mouse_down = ImGui::IsMouseDown( 0 );

    if ( ( m_graph.mouse_captured == MOUSE_CAPTURED_ZOOM ) ||
         ( m_graph.mouse_captured == MOUSE_CAPTURED_SELECT_AREA ) )
    {
        // shift + click: zoom area
        int64_t event_ts0 = gi.screenx_to_ts( m_graph.mouse_capture_pos.x );
        int64_t event_ts1 = gi.screenx_to_ts( gi.mouse_pos.x );

        if ( event_ts0 > event_ts1 )
            std::swap( event_ts0, event_ts1 );

        if ( is_mouse_down )
        {
            std::string time_buf0 = ts_to_timestr( event_ts0, m_eventlist.tsoffset );
            std::string time_buf1 = ts_to_timestr( event_ts1 - event_ts0 );

            // Show tooltip with starting time and length of selected area.
            ImGui::SetTooltip( "%s (%s ms)", time_buf0.c_str(), time_buf1.c_str() );
        }
        else if ( m_graph.mouse_captured == MOUSE_CAPTURED_ZOOM )
        {
            m_graph.zoom_loc = std::make_pair( m_graph.start_ts, m_graph.length_ts );

            m_graph.start_ts = event_ts0 - m_eventlist.tsoffset;
            m_graph.length_ts = event_ts1 - event_ts0;
            m_graph.recalc_timebufs = true;
        }
    }
    else if ( m_graph.mouse_captured == MOUSE_CAPTURED_PAN )
    {
        // click: pan
        if ( is_mouse_down && imgui_mousepos_valid( gi.mouse_pos ) )
        {
            float dx = gi.mouse_pos.x - m_graph.mouse_capture_pos.x;
            int64_t tsdiff = gi.dx_to_ts( dx );

            m_graph.start_ts -= tsdiff;
            m_graph.recalc_timebufs = true;

            m_graph.start_y += gi.mouse_pos.y - m_graph.mouse_capture_pos.y;

            m_graph.mouse_capture_pos = gi.mouse_pos;
        }
    }

    if ( !is_mouse_down )
    {
        // Mouse is no longer down, uncapture mouse...
        m_graph.mouse_captured = MOUSE_NOT_CAPTURED;
        ImGui::CaptureMouseFromApp( false );
    }

}

void TraceWin::graph_set_mouse_tooltip( class graph_info_t &gi, int64_t mouse_ts )
{
    std::string time_buf = "Time: " + ts_to_timestr( mouse_ts, m_eventlist.tsoffset );
    bool sync_event_list_to_graph = s_opts().getb( OPT_SyncEventListToGraph ) &&
            s_opts().getb( OPT_ShowEventList );

    m_eventlist.highlight_ids.clear();

    const std::vector< uint32_t > *vblank_locs = m_trace_events.get_tdopexpr_locs( "$name=drm_vblank_event" );
    if ( vblank_locs )
    {
        int64_t prev_vblank_ts = INT64_MAX;
        int64_t next_vblank_ts = INT64_MAX;
        int eventid = ts_to_eventid( mouse_ts );
        size_t idx = vec_find_eventid( *vblank_locs, eventid );
        size_t idxmax = std::min< size_t >( idx + 20, vblank_locs->size() );

        for ( idx = ( idx > 10 ) ? ( idx - 10 ) : 0; idx < idxmax; idx++ )
        {
            trace_event_t &event = get_event( vblank_locs->at( idx ) );

            if ( s_opts().getcrtc( event.crtc ) )
            {
                if ( event.ts < mouse_ts )
                {
                    if ( mouse_ts - event.ts < prev_vblank_ts )
                        prev_vblank_ts = mouse_ts - event.ts;
                }
                if ( event.ts > mouse_ts )
                {
                    if ( event.ts - mouse_ts < next_vblank_ts )
                        next_vblank_ts = event.ts - mouse_ts;
                }
            }
        }

        if ( prev_vblank_ts != INT64_MAX )
            time_buf += "\nPrev vblank: -" + ts_to_timestr( prev_vblank_ts, 0, 2 ) + "ms";
        if ( next_vblank_ts != INT64_MAX )
            time_buf += "\nNext vblank: " + ts_to_timestr( next_vblank_ts, 0, 2 ) + "ms";
    }

    if ( graph_marker_valid( 0 ) )
        time_buf += "\nMarker A: " + ts_to_timestr( m_graph.ts_markers[ 0 ] - mouse_ts, 0, 2 ) + "ms";
    if ( graph_marker_valid( 1 ) )
        time_buf += "\nMarker B: " + ts_to_timestr( m_graph.ts_markers[ 1 ] - mouse_ts, 0, 2 ) + "ms";

    m_graph.hovered_eventid = INVALID_ID;
    if ( !gi.hovered_items.empty() )
    {
        // Sort hovered items array by id
        std::sort( gi.hovered_items.begin(), gi.hovered_items.end(),
                   [=]( const graph_info_t::hovered_t& lx, const graph_info_t::hovered_t &rx )
        {
            return lx.eventid < rx.eventid;
        } );

        time_buf += "\n";

        // Show tooltip with the closest events we could drum up
        for ( graph_info_t::hovered_t &hov : gi.hovered_items )
        {
            trace_event_t &event = get_event( hov.eventid );

            m_eventlist.highlight_ids.push_back( event.id );

            // Add event id and distance from cursor to this event
            time_buf += string_format( "\n%u %c%sms",
                                       hov.eventid, hov.neg ? '-' : ' ',
                                       ts_to_timestr( hov.dist_ts, 0, 4 ).c_str() );

            // If this isn't an ftrace print event, add the event name
            if ( !event.is_ftrace_print() )
                time_buf += std::string( " " ) + event.name;

            // If this is a vblank event, add the crtc
            if ( event.crtc >= 0 )
                time_buf += std::to_string( event.crtc );

            // Add colored string for ftrace print events
            if ( event.is_ftrace_print() )
            {
                const char *buf = get_event_field_val( event, "buf" );

                if ( buf[ 0 ] )
                    time_buf += " " + s_textclrs().ftraceprint_str( buf );
            }
        }

        // Mark the first event in the list as our hovered graph event
        m_graph.hovered_eventid = gi.hovered_items[ 0 ].eventid;

        if ( sync_event_list_to_graph && !m_eventlist.do_gotoevent )
        {
            m_eventlist.do_gotoevent = true;
            m_eventlist.goto_eventid = gi.hovered_items[ 0 ].eventid;
        }
    }

    if ( is_valid_id( gi.hovered_fence_signaled ) )
    {
        const trace_event_t &event_hov = get_event( gi.hovered_fence_signaled );
        std::string context = get_event_gfxcontext_str( event_hov );
        const std::vector< uint32_t > *plocs = m_trace_events.get_gfxcontext_locs( context.c_str() );

        time_buf += string_format( "\n\n%s", event_hov.user_comm );

        for ( uint32_t id : *plocs )
        {
            const trace_event_t &event = get_event( id );
            const char *name = event.get_timeline_name( event.name );
            std::string timestr = ts_to_timestr( event.duration, 0, 4 );

            if ( gi.hovered_items.empty() )
                m_eventlist.highlight_ids.push_back( id );

            time_buf += string_format( "\n  %u %s duration: %s", event.id, name,
                                       s_textclrs().ftraceprint_str( timestr + "ms" ).c_str() );
        }

        if ( sync_event_list_to_graph && !m_eventlist.do_gotoevent )
        {
            // Sync event list to first event id in this context
            m_eventlist.do_gotoevent = true;
            m_eventlist.goto_eventid = plocs->at( 0 );
        }
    }

    ImGui::SetTooltip( "%s", time_buf.c_str() );
}

void TraceWin::graph_handle_mouse( graph_info_t &gi )
{
    // If we've got an active popup menu, render it.
    if ( m_graph.popupmenu )
    {
        m_graph.popupmenu = TraceWin::graph_render_popupmenu( gi );
        return;
    }

    m_graph.ts_marker_mouse = -1;

    // Check if mouse if over our graph and we've got focus
    m_graph.is_mouse_over = gi.mouse_pos_in_graph() &&
                         ImGui::IsRootWindowOrAnyChildFocused();

    // If we don't own the mouse and we don't have focus, bail.
    if ( !m_graph.mouse_captured && !m_graph.is_mouse_over )
        return;

    if ( m_graph.mouse_captured )
    {
        graph_handle_mouse_captured( gi );
        return;
    }

    // Mouse is over our active graph window
    {
        int64_t mouse_ts = gi.screenx_to_ts( gi.mouse_pos.x );

        m_graph.ts_marker_mouse = mouse_ts;

        // Set the tooltip
        graph_set_mouse_tooltip( gi, mouse_ts );

        // Check for clicking, wheeling, etc.
        if ( ImGui::IsMouseClicked( 0 ) )
        {
            if ( ImGui::GetIO().KeyCtrl )
            {
                // ctrl + click: select area
                m_graph.mouse_captured = MOUSE_CAPTURED_SELECT_AREA;
                ImGui::CaptureMouseFromApp( true );
                m_graph.mouse_capture_pos = gi.mouse_pos;
            }
            else if ( ImGui::GetIO().KeyShift )
            {
                // shift + click: zoom
                m_graph.mouse_captured = MOUSE_CAPTURED_ZOOM;
                ImGui::CaptureMouseFromApp( true );
                m_graph.mouse_capture_pos = gi.mouse_pos;
            }
            else
            {
                // click: pan
                m_graph.mouse_captured = MOUSE_CAPTURED_PAN;
                ImGui::CaptureMouseFromApp( true );
                m_graph.mouse_capture_pos = gi.mouse_pos;
            }
        }
        else if ( ImGui::IsMouseClicked( 1 ) )
        {
            // right click: popup menu
            m_graph.popupmenu = true;

            m_graph.rows_hidden_rows = m_graph.rows.get_hidden_rows_list();
            m_graph.new_row_errstr = "";

            ImGui::OpenPopup( "GraphPopup" );
        }
        else if ( ImGui::GetIO().MouseWheel )
        {
            bool zoomin = ( ImGui::GetIO().MouseWheel > 0.0f );

            graph_zoom( mouse_ts, gi.ts0, zoomin );
        }
    }
}

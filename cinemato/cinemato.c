#include <module.h>
#include <dryos.h>
#include <menu.h>
#include <config.h>
#include <lens.h>
#include <bmp.h>
#include <powersave.h>


// focus sequence speed entry count:
#define CINEMATO_FOCUS_SEQUENCE_SPEED_COUNT 5

// settings file name:
#define CINEMATO_SETTINGS_FILE "ML/SETTINGS/cinemato.cfg"


// focus sequence speed structure definition:
typedef struct
{
    // speed value count:
    int count;
    
    // current index array:
    int index;
    
    // displayed label:
    const char * const labels[ CINEMATO_FOCUS_SEQUENCE_SPEED_COUNT ];
    
    // related sleep data (ms):
    const int data[ CINEMATO_FOCUS_SEQUENCE_SPEED_COUNT ];
}
Cinemato_FocusSequenceSpeed;


// focus sequence point definition:
typedef struct
{
    // absolute focus step value:
    int steps;
    
    // transition speed (index):
    int speedIndex;
    
    // lens distance, in mm:
    int distance;
}
Cinemato_FocusSequencePoint;


// focus sequence structure definifion:
typedef struct
{
    // sequence length:
    int length;
    
    // current index in sequence:
    int index;
    
    Cinemato_FocusSequenceSpeed speed;
    
    // sequence data:
    Cinemato_FocusSequencePoint data[ 100 ]; // max 100 points in the sequence
}
Cinemato_FocusSequence;


// cinematographer global data structure definition:
typedef struct
{
    // is the cinematographer task currently running ?
    bool isTaskRunning;
    
    // is the cinematographer mode currently enabled ?
    bool isEnabled;
    
    // are we in PLAY mode ? (or in EDIT mode then)
    bool isModePlay; 
    
    // are we currently doing a focus transition?
    bool isInTransition;
    
    // does the camera screen display currently activated?
    bool isDisplayOn;
    
    // current asbolute focus step value:
    // note: steps are anyway related to the initial value at camera startup
    int focusSteps;
    
    Cinemato_FocusSequence focusSequence;
}
Cinemato_Data;


// global data values with default values:
static Cinemato_Data g_cinemato_data = {
    .isTaskRunning  = false,
    .isEnabled      = false,
    .isModePlay     = true,
    .isInTransition = false,
    .isDisplayOn    = true,
    .focusSteps     = 0,
    .focusSequence  = {
        .length         = 0,
        .index          = 0,
        .speed          = {
            .count          = CINEMATO_FOCUS_SEQUENCE_SPEED_COUNT,
            .index          = 0,
            .labels         = { "fastest", "fast", "medium", "slow", "slowest" },
            .data           = { 0, 10, 50, 100, 200 }
        }
    }
};


// read configuration file (if any):
static void Cinemato_ReadConfig()
{
    // notation shortcuts:
    Cinemato_Data * const pData = &g_cinemato_data;
    Cinemato_FocusSequence * const pFocusSequence = &pData->focusSequence;
    
    // open settings file for read:
    FILE * pFile = FIO_OpenFile( CINEMATO_SETTINGS_FILE, O_RDONLY );
    if( pFile == 0 )
        return;
    FIO_ReadFile( pFile, &pFocusSequence->index, sizeof( int ) );
    FIO_ReadFile( pFile, &pFocusSequence->length, sizeof( int ) );
    FIO_ReadFile( pFile, pFocusSequence->data, pFocusSequence->length * 3 * sizeof( int ) );
    FIO_CloseFile( pFile );
}


// write configuration file:
static void Cinemato_WriteConfig()
{
    // notation shortcuts:
    const Cinemato_Data * const pData = &g_cinemato_data;
    const Cinemato_FocusSequence * const pFocusSequence = &pData->focusSequence;
    
    // open settings file for write:
    FIO_RemoveFile( CINEMATO_SETTINGS_FILE );
    FILE * pFile = FIO_CreateFile( CINEMATO_SETTINGS_FILE );
    FIO_WriteFile( pFile, &pFocusSequence->index, sizeof( int ) );
    FIO_WriteFile( pFile, &pFocusSequence->length, sizeof( int ) );
    FIO_WriteFile( pFile, pFocusSequence->data, pFocusSequence->length * 3 * sizeof( int ) );
    FIO_CloseFile( pFile );
}


// specific print function:
static void Cinemato_Print( const char * _fmt, ... )
{    
    // unfold arguments regarding format:
    va_list args;
    static char data[ 42 ]; // max line length
    va_start( args, _fmt );
    int dataLen = sizeof( data );
    int written = vsnprintf( data, dataLen - 1, _fmt, args );
    va_end( args );
    
    // complement data with spaces regarding line width:
    memset( data + written, ' ', dataLen - 1 - written );
    data[ dataLen - 1 ] = 0;
    
    // draw label with data, white font over black background:
    static uint32_t fontspec = FONT( FONT_SMALL, COLOR_WHITE, COLOR_BLACK );
    int x = 0; // stick on the left
    int y = 100; // 100px from top
    bmp_puts( fontspec, &x, &y, data );
}


// overlay task conditional print:
static void Cinemato_OverlayTask_Print()
{
    // notation shortcuts:
    const Cinemato_Data * const pData = &g_cinemato_data;
    const Cinemato_FocusSequence * const pFocusSequence = &pData->focusSequence;
    const Cinemato_FocusSequenceSpeed * const pFocusSequenceSpeed = &pFocusSequence->speed;
    
    // cinematographer mode currently disabled, just display:
    // [idle]
    if( !pData->isEnabled ) {
        Cinemato_Print( "[idle]" );
        return;
    }
    
    // we're in EDIT mode, display the current recorded point count (1),
    // alongside current lens distance (2) and speed (3) to be queued, under the form:
    // [edit] (1) < (2)mm (3)
    if( !pData->isModePlay ) {
        const int lensDistance = lens_info.focus_dist * 10;
        const char * const currentSpeed = pFocusSequenceSpeed->labels[ pFocusSequenceSpeed->index ];
        Cinemato_Print( "[edit] %d < %dmm (%s)", pFocusSequence->length, lensDistance, currentSpeed );
        return;
    }
    
    // we're in PLAY mode:
    const int lensDistance = lens_info.focus_dist * 10;
        
    // we're not in transition, display the focus lens index (1) over the sequence length (2),
    // alongside both current lens distance (3), target distance of next reachable point (4)
    // and transition speed (5), under the form:
    // [play] (1)/(2) (3)mm > (4)mm (5)
    if( !pData->isInTransition ) {
        int targetIndex = pFocusSequence->index + 1;
        if( targetIndex > pFocusSequence->length )
            targetIndex = 1;
        const Cinemato_FocusSequencePoint * const pTargetPoint = &pFocusSequence->data[ targetIndex - 1 ];
        const char * const targetSpeed = pFocusSequenceSpeed->labels[ pTargetPoint->speedIndex ];
        Cinemato_Print( "[play] %d/%d %dmm > %dmm (%s)", pFocusSequence->index, pFocusSequence->length, lensDistance, pTargetPoint->distance, targetSpeed );
        return;
    }
    
    // we're in transition, display the focus lens index (1) source and the focus lens
    // destination (2) alongside current lens distance (3) and transition speed (4), under the form:
    // [play] (1)>(2) (3)mm (4)
    // note: the focus lens index was already incremented at this point
    int sourceIndex = pFocusSequence->index - 1;
    if( sourceIndex == 0 )
        sourceIndex = pFocusSequence->length;
    const Cinemato_FocusSequencePoint * const pTargetPoint = &pFocusSequence->data[ pFocusSequence->index - 1 ];
    const char * const targetSpeed = pFocusSequenceSpeed->labels[ pTargetPoint->speedIndex ];
    Cinemato_Print( "[play] %d>%d %dmm (%s)", sourceIndex, pFocusSequence->index, lensDistance, targetSpeed );
}


// overlay task:
static void Cinemato_OverlayTask()
{
    // notation shortcuts:
    Cinemato_Data * const pData = &g_cinemato_data;
    
    // read config file is any:
    Cinemato_ReadConfig();
    
    // task is now running:
    pData->isTaskRunning = true;
    
    // loop indefinitely in order to deal with cinematographer mode display overlays:
    for(;;) {
        Cinemato_OverlayTask_Print();
        
        // breathe a little to let other tasks do their job properly:
        msleep( 100 );
    }
}


// generic focus update:
static void Cinemato_UpdateFocus( const int _steps, const int _waitMs )
{
    // notation shortcuts:
    Cinemato_Data * const pData = &g_cinemato_data;
    
    // start transition:
    pData->isInTransition = true;
    
    // change lens focus accurately:
    lens_focus( _steps, 1, 1, _waitMs );
    
    // update asbolute focus step value:
    pData->focusSteps += _steps;
    
    // stop transition:
    pData->isInTransition = false;
}


// menu definition:
static struct menu_entry g_cinemato_menu[] = { {
    .name    = "Cinematographer",
    .select  = run_in_separate_task, // DryOS task, running in parallel
    .priv    = Cinemato_OverlayTask,
    .help    = "Start cinematographer mode with focus sequencing",
} };


// key handler:
static unsigned int Cinemato_KeyHandler( const unsigned int _key )
{
    // notation shortcuts:
    Cinemato_Data * const pData = &g_cinemato_data;
    Cinemato_FocusSequence * const pFocusSequence = &pData->focusSequence;
    Cinemato_FocusSequenceSpeed * const pFocusSequenceSpeed = &pFocusSequence->speed;
    
    // task is not running, bypass:
    if( !pData->isTaskRunning )
        return 1;
    
    // [INFO] push button allows to enable/disable cinematographer mode:
    if( _key == MODULE_KEY_INFO ) {
        pData->isEnabled =! pData->isEnabled;
        return 0;
    }
    
    // cinematographer mode is disabled, no need to track other keys:
    if( !pData->isEnabled )
        return 1;
    
    // quick focus change constant:
    const int manualQuickFocusChange = 10; // 10 steps movement
    
    // custom keys:
    switch( _key ) {
        
        // [UP] push button allows to precisely decrease the lens focus:
        case MODULE_KEY_PRESS_UP:
            Cinemato_UpdateFocus( -1, 0 );
            return 0;
            
        // [RIGHT] push button allows to quickly decrease the lens focus:
        case MODULE_KEY_PRESS_RIGHT:
            Cinemato_UpdateFocus( -manualQuickFocusChange, 0 );
            return 0;
            
        // [DOWN] push button allows to precisely increase the lens focus:
        case MODULE_KEY_PRESS_DOWN:
            Cinemato_UpdateFocus( 1, 0 );
            return 0;
            
        // [LEFT] push button allows to quickly increase the lens focus:
        case MODULE_KEY_PRESS_LEFT:
            Cinemato_UpdateFocus( manualQuickFocusChange, 0 );
            return 0;
            
        // [Q] push button allows to toggle between PLAY and EDIT modes:
        case MODULE_KEY_Q:
            pData->isModePlay = !pData->isModePlay;
        
            // PLAY mode activated, we set the current sequence index at the end of the list:
            if( pData->isModePlay ) {
                pFocusSequence->index = pFocusSequence->length;
                
                // write config file (index updated):
                Cinemato_WriteConfig();
                return 0;
            }
            
            // EDIT mode activated, we reset the list length
            pFocusSequence->length = 0;
            return 0;
            
        // [SET] push button allows to record or replay a sequence points:
        case MODULE_KEY_PRESS_SET:
            // we're in EDIT mode, we just need to save the current lens focus information in the list:
            if( !pData->isModePlay ) {
                Cinemato_FocusSequencePoint * pPoint = &pFocusSequence->data[ pFocusSequence->length++ ];
                pPoint->steps = pData->focusSteps;
                pPoint->speedIndex = pFocusSequenceSpeed->index;
                pPoint->distance = lens_info.focus_dist * 10;
                
                // write config file (sequence updated):
                Cinemato_WriteConfig();
                return 0;
            }
            
            // we're in PLAY mode, but the list is empty:
            if( pFocusSequence->length == 0 )
                return 0;
            
            // we reach the end of the list, jump to the first point:
            if( ++pFocusSequence->index > pFocusSequence->length )
                pFocusSequence->index = 1;
            
            // write config file (index updated):
            Cinemato_WriteConfig();
            
            // do lens focus transition from the current absolute position to the one we're targeting:
            const Cinemato_FocusSequencePoint * const pPoint = &pFocusSequence->data[ pFocusSequence->index - 1 ];
            int steps = pPoint->steps - pData->focusSteps;
            int waitMs = pFocusSequenceSpeed->data[ pPoint->speedIndex ];
            Cinemato_UpdateFocus( steps, waitMs );
            return 0;
            
        // [RATE] push button allows to change the sequence speed
        case MODULE_KEY_RATE:
            // increase the current speed index, loop if we're reaching the end of the list:
            if( ++pFocusSequenceSpeed->index == CINEMATO_FOCUS_SEQUENCE_SPEED_COUNT )
                pFocusSequenceSpeed->index = 0;
            return 0;
            
        // [PLAY] push button allows to toggle the camera display on/off:
        case MODULE_KEY_PLAY:
            pData->isDisplayOn = !pData->isDisplayOn;
            
            // turn on the camera display:
            if( pData->isDisplayOn ) {
                display_on();
                return 0;
            }
            
            // turn off the camera display:
            display_off();
            return 0;
            
        // bypass by default:
        default:
            return 1;
    }
}


// init:
static unsigned int Cinemato_Init()
{    
    // add cinematographer mode to Movie menu:
    menu_add( "Movie", g_cinemato_menu, COUNT( g_cinemato_menu ) );
    return 0;
}


// deinit:
static unsigned int Cinemato_DeInit()
{
    return 0;
}


MODULE_INFO_START()
    MODULE_INIT( Cinemato_Init )
    MODULE_DEINIT( Cinemato_DeInit )
MODULE_INFO_END()

MODULE_CBRS_START()
    MODULE_CBR( CBR_KEYPRESS, Cinemato_KeyHandler, 0 )
MODULE_CBRS_END()
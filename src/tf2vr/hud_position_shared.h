#pragma once

// Shared HUD position data structure for communication between game and compositor
struct SharedHUDPositionData {
    // HUD quad corners in world space (Source engine coordinates)
    float viewer_pos[3];      // Viewer position (camera/eye position)
    float upper_left[3];      // Upper-left corner of HUD quad
    float upper_right[3];     // Upper-right corner of HUD quad
    float lower_left[3];      // Lower-left corner of HUD quad
    float lower_right[3];     // Lower-right corner of HUD quad
    
    // Timing and validity
    double last_update_time;  // When this data was last updated
    bool is_valid;            // Whether the position data is valid/fresh
    bool is_custom_bounds;    // Whether using custom bounds (menus) vs dynamic bounds (gameplay HUD)
    
    // Additional metadata
    int frame_number;         // Frame number when this position was captured
    float world_scale;        // VR world scale factor applied
};

// Function for VRCompositor to access current HUD position data
SharedHUDPositionData GetCurrentHUDPosition();

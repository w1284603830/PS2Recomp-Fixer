#pragma once

// DoD: anamorphic 16:9 projection with separate 2D layout. Configuration is
// fixed before the game thread starts; this never changes the camera's position.
namespace dodwide
{
    inline bool enabled = false;
}

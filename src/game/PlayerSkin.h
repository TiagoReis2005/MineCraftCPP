#pragma once

#include <string>

namespace mc {

struct PlayerSkin {
    std::string texturePath; // absolute path to the 64x64 skin PNG (empty if none found)
    bool        slim = false; // true = Alex model (3px arms), false = Steve (4px arms)
    std::string name;         // skin name, for display
};

// Chooses the player's skin: a `preferred` skin name (saved in options.txt) wins when it
// still exists; else any PNG under <playerDir>/custom/ (slim if the filename says so);
// else a random default from <playerDir>/{slim,wide}/.
PlayerSkin pickPlayerSkin(const std::string& playerDir, const std::string& preferred = "");

// Joint-cap colors for the split-limb model (player.json forearm/shin bones): averages
// the ring of texels at each limb's elbow/knee seam (LOD-style) and writes the result
// into 4 reserved dead pixels of the skin — (0,0) rightArm, (1,0) leftArm, (2,0)
// rightLeg, (3,0) leftLeg — which the geometry's joint cap faces sample as flat color.
// 64x64 RGBA skins only (no-op otherwise); call on the CPU pixels before GPU upload.
void patchSkinJointCaps(unsigned char* rgba, int w, int h);

} // namespace mc

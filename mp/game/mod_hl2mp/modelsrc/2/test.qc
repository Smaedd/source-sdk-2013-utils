$scale 1.0
$modelname	"props/bhop_greything/booster.mdl"
$staticprop
$body booster "booster.smd" // Test
$collisionmodel "booster_hull.smd" {

}
$surfaceprop "metal" 
$contents "solid"
$cdmaterials "models/props/bhop_greything"
$sequence idle "booster.smd" activity ACT_IDLE -1 fps 1

//$include	"booster_wwmt_custom.qci"

<?php
/*
 * Copyright 2004 - 15 Matt Brubeck, Gale Andrews
 * This file is licensed under a Creative Commons license:
 * http://creativecommons.org/licenses/by/2.0/
 */
  require_once "main.inc.php";
  $pageId = "screenshots";
  $pageTitle = _("Screenshots");
  include "../include/header.inc.php";
?>

<h2><?=$pageTitle?></h2>

<p><?=_('Click any screenshot for a larger version.')?></p>

<p class="screenshot"><a href="images/audacity-linux.png"><img alt="" src="images/audacity-linux-small.jpg"><?=_("Audacity running on GNU/Linux")?></a></p>

<p class="screenshot"><a href="images/audacity-windows.png"><img alt="" src="images/audacity-windows-small.jpg"><?=_("Audacity running on Windows")?></a></p>

<p class="screenshot"><a href="images/audacity-macosx.png"><img alt="" src="images/audacity-macosx-small.jpg"><?=_("Audacity running on Mac OS X")?></a></p>

<p class="screenshot"><a href="images/effect-noise-reduction.png"><img alt="" src="images/effect-noise-reduction-small.png"><?=_("Noise Reduction effect")?></a></p>

<p class="screenshot"><a href="images/effect-change-pitch.png"><img alt="" src="images/effect-change-pitch-small.png"><?=_("Change Pitch effect")?></a></p>

<p class="screenshot"><a href="images/effect-change-tempo.png"><img alt="" src="images/effect-change-tempo-small.png"><?=_("Change Tempo effect")?></a></p>

<p class="screenshot"><a href="images/recording.png"><img alt="" src="images/recording-small.jpg"><?=_("Recording with Audacity")?></a></p>

<?php
  include "../include/footer.inc.php";
?>

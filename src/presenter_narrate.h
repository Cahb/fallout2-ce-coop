#ifndef FALLOUT_PRESENTER_NARRATE_H_
#define FALLOUT_PRESENTER_NARRATE_H_

namespace fallout {

// Installs the narrating presenter (see presenter_narrate.cc) as the current
// presenter. Called by serverRun() when F2_NARRATE is set in the environment.
void presenterInstallNarrate();

} // namespace fallout

#endif /* FALLOUT_PRESENTER_NARRATE_H_ */

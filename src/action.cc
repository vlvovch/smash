/*
 *
 *    Copyright (c) 2014
 *      SMASH Team
 *
 *    GNU General Public License (GPLv3 or later)
 *
 */

#include "include/action.h"

#include <assert.h>
#include <sstream>

#include "include/angles.h"
#include "include/constants.h"
#include "include/logging.h"
#include "include/random.h"
#include "include/resonances.h"


namespace Smash {

Action::Action(const ParticleList &in_part, float time_of_execution)
    : incoming_particles_(in_part), time_of_execution_(time_of_execution),
      total_weight_(0.) {}

Action::Action(Action &&a)
    : incoming_particles_(std::move(a.incoming_particles_)),
      outgoing_particles_(std::move(a.outgoing_particles_)),
      subprocesses_(std::move(a.subprocesses_)),
      time_of_execution_(a.time_of_execution_),
      total_weight_(a.total_weight_) {}

Action::~Action() = default;

float Action::weight() const {
  return total_weight_;
}

void Action::add_process(ProcessBranch p) {
  total_weight_ += p.weight();
  subprocesses_.emplace_back(std::move(p));
}

void Action::add_processes(ProcessBranchList pv) {
  if (subprocesses_.empty()) {
    subprocesses_ = std::move(pv);
    for (auto &proc : subprocesses_) {
      total_weight_ += proc.weight();
    }
  } else {
    subprocesses_.reserve(subprocesses_.size() + pv.size());
    for (auto &proc : pv) {
      total_weight_ += proc.weight();
      subprocesses_.emplace_back(std::move(proc));
    }
  }
}

bool Action::is_valid(const Particles &particles) const {
  for (const auto &part : incoming_particles_) {
    // Check if the particles still exists. If it decayed or scattered
    // inelastically it is gone.
    if (!particles.has_data(part.id())) {
      return false;
    }
    // If the particle has scattered elastically, its id_process has changed and
    // we consider it invalid.
    if (particles.data(part.id()).id_process() != part.id_process()) {
      return false;
    }
  }
  return true;
}

ParticleList Action::incoming_particles() const {
  ParticleList l;
  for (const auto &part : incoming_particles_) {
    l.emplace_back(part);
  }
  return std::move(l);
}

ThreeVector Action::get_interaction_point() {
  // Estimate for the interaction point in the calculational frame
  ThreeVector interaction_point = ThreeVector(0.0, 0.0, 0.0);
  for (const auto &part : incoming_particles_) {
    interaction_point += part.position().threevec();
  }
  interaction_point /= incoming_particles_.size();
  return interaction_point;
}


ParticleList Action::choose_channel() {
  const auto &log = logger<LogArea::Action>();
  double random_interaction = Random::canonical();
  float interaction_probability = 0.0;
  /* Loop through all subprocesses and select one by Monte Carlo, based on
   * their weights.  */
  for (const auto &proc : subprocesses_) {
    if (proc.particle_types().size() < 1 ||
        proc.particle_types()[0]->pdgcode() == PdgCode::invalid()) {
      continue;
    }
    interaction_probability += proc.weight() / total_weight_;
    if (random_interaction <= interaction_probability) {
      return proc.particle_list();
    }
  }
  /* Should never get here. */
  log.fatal(source_location, "Problem in choose_channel: ",
            subprocesses_.size(), " ", interaction_probability, " ",
            total_weight_, "\n", *this);
  throw std::runtime_error("problem in choose_channel");
}


void Action::sample_cms_momenta() {
  const auto &log = logger<LogArea::Action>();
  /* This function only operates on 2-particle final states. */
  assert(outgoing_particles_.size() == 2);

  ParticleData *p_a = &outgoing_particles_[0];
  ParticleData *p_b = &outgoing_particles_[1];

  const ParticleType &t_a = p_a->type();
  const ParticleType &t_b = p_b->type();

  double mass_a = t_a.mass();
  double mass_b = t_b.mass();

  const double cms_energy = sqrt_s();

  if (cms_energy < t_a.minimum_mass() + t_b.minimum_mass()) {
    throw InvalidResonanceFormation("resonance_formation: not enough energy! " +
      std::to_string(cms_energy) + " " + std::to_string(t_a.minimum_mass()) +
      " " + std::to_string(t_b.minimum_mass()) + " " +
      p_a->pdgcode().string() + " " + p_b->pdgcode().string());
  }

  /* If one of the particles is a resonance, sample its mass. */
  /* TODO: Other particle assumed stable! */
  if (!t_a.is_stable()) {
    mass_a = sample_resonance_mass(t_a, t_b, cms_energy);
  } else if (!t_b.is_stable()) {
    mass_b = sample_resonance_mass(t_b, t_a, cms_energy);
  }

  double energy_a = (cms_energy * cms_energy + mass_a * mass_a - mass_b * mass_b) /
                   (2.0 * cms_energy);
  double momentum_radial = std::sqrt(energy_a * energy_a - mass_a * mass_a);
  if (!(momentum_radial > 0.0)) {
    log.warn("radial momenta ", momentum_radial);
  }
  /* TODO : Angles should be sampled from differential cross section
   * of this process. */
  Angles phitheta;
  phitheta.distribute_isotropically();
  if (!(energy_a > mass_a)) {
    log.info("Particle ", t_a.pdgcode(), " radial momenta ", momentum_radial,
             phitheta);
    log.info("Etot: ", cms_energy, " m_a: ", mass_a, " m_b: ", mass_b, " E_a: ",
             energy_a);
  }

  p_a->set_4momentum(FourVector(energy_a, phitheta.threevec() * momentum_radial));
  p_b->set_4momentum(FourVector(cms_energy - energy_a,
                               -phitheta.threevec() * momentum_radial));

  log.debug("p_a: ", *p_a, "\np_b: ", *p_b);
}


void Action::check_conservation(const size_t &id_process) const {
  const auto &log = logger<LogArea::Action>();
  /* Check momentum conservation */
  FourVector momentum_difference;
  for (const auto &part : incoming_particles_) {
    momentum_difference += part.momentum();
  }
  for (const auto &p : outgoing_particles_) {
    momentum_difference -= p.momentum();
  }

  /* TODO: throw an exception */
  if (fabs(momentum_difference.x0()) > really_small) {
    log.warn("Process ", id_process, "\nE conservation violation ",
             momentum_difference.x0());
  }
  if (fabs(momentum_difference.x1()) > really_small) {
    log.warn("px conservation violation ", momentum_difference.x1());
  }
  if (fabs(momentum_difference.x2()) > really_small) {
    log.warn("py conservation violation ", momentum_difference.x2());
  }
  if (fabs(momentum_difference.x3()) > really_small) {
    log.warn("pz conservation violation ", momentum_difference.x3());
  }

  // TODO: check other conservation laws (baryon number etc)
}

std::ostream &operator<<(std::ostream &out, const ActionList &actions) {
  out << "ActionList {\n";
  for (const auto &a : actions) {
    out << "- " << a << '\n';
  }
  return out << '}';
}

}  // namespace Smash

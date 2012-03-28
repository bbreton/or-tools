// Copyright 2010-2012 Google
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.
//

#include <math.h>
#include <stddef.h>
#include "base/hash.h"
#include <limits>
#include <string>
#include <utility>
#include <vector>

#include "base/commandlineflags.h"
#include "base/integral_types.h"
#include "base/logging.h"
#include "base/scoped_ptr.h"
#include "base/stringprintf.h"
#include "base/timer.h"
#include "base/concise_iterator.h"
#include "base/hash.h"
#include "linear_solver/linear_solver.h"

#if defined(USE_GLPK)

extern "C" {
  #include "glpk.h"
}

DECLARE_double(solver_timeout_in_seconds);
DECLARE_string(solver_write_model);

namespace operations_research {

// Class to store information gathered in the callback
class GLPKInformation {
 public:
  explicit GLPKInformation(bool maximize): num_all_nodes_(0) {
    ResetBestObjectiveBound(maximize);
  }
  void Reset(bool maximize) {
    num_all_nodes_ = 0;
    ResetBestObjectiveBound(maximize);
  }
  void ResetBestObjectiveBound(bool maximize) {
    if (maximize) {
      best_objective_bound_ = std::numeric_limits<double>::infinity();
    } else {
      best_objective_bound_ = -std::numeric_limits<double>::infinity();
    }
  }
  int num_all_nodes_;
  double best_objective_bound_;
};



// Function to be called in the GLPK callback
void GLPKGatherInformationCallback(glp_tree* tree, void* info) {
  CHECK_NOTNULL(tree);
  CHECK_NOTNULL(info);
  GLPKInformation* glpk_info = reinterpret_cast<GLPKInformation*>(info);
  switch (glp_ios_reason(tree)) {
    // The best bound and the number of nodes change only when GLPK
    // branches, generates cuts or finds an integer solution.
    case GLP_ISELECT:
    case GLP_IROWGEN:
    case GLP_IBINGO: {
      // Get total number of nodes
      glp_ios_tree_size(tree, NULL, NULL, &glpk_info->num_all_nodes_);
      // Get best bound
      int node_id = glp_ios_best_node(tree);
      if (node_id > 0) {
        glpk_info->best_objective_bound_ = glp_ios_node_bound(tree, node_id);
      }
      break;
    }
    default:
      break;
  }
}

// ----- GLPK Solver -----

class GLPKInterface : public MPSolverInterface {
 public:
  // Constructor that takes a name for the underlying glpk solver.
  GLPKInterface(MPSolver* const solver, bool mip);
  ~GLPKInterface();

  // Sets the optimization direction (min/max).
  virtual void SetOptimizationDirection(bool maximize);

  // ----- Solve -----
  // Solve the problem using the parameter values specified.
  virtual MPSolver::ResultStatus Solve(const MPSolverParameters& param);

  // ----- Model modifications and extraction -----
  // Resets extracted model
  virtual void Reset();

  // Modify bounds.
  virtual void SetVariableBounds(int var_index, double lb, double ub);
  virtual void SetVariableInteger(int var_index, bool integer);
  virtual void SetConstraintBounds(int row_index, double lb, double ub);

  // Add Constraint incrementally.
  void AddRowConstraint(MPConstraint* const ct);
  // Add variable incrementally.
  void AddVariable(MPVariable* const var);
  // Change a coefficient in a constraint.
  virtual void SetCoefficient(MPConstraint* const constraint,
                              const MPVariable* const variable,
                              double new_value,
                              double old_value);
  // Clear a constraint from all its terms.
  virtual void ClearConstraint(MPConstraint* const constraint);
  // Change a coefficient in the linear objective
  virtual void SetObjectiveCoefficient(const MPVariable* const variable,
                                       double coefficient);
  // Change the constant term in the linear objective.
  virtual void SetObjectiveOffset(double value);
  // Clear the objective from all its terms.
  virtual void ClearObjective();

  // ------ Query statistics on the solution and the solve ------
  // Number of simplex iterations
  virtual int64 iterations() const;
  // Number of branch-and-bound nodes. Only available for discrete problems.
  virtual int64 nodes() const;
  // Best objective bound. Only available for discrete problems.
  virtual double best_objective_bound() const;

  // Returns the basis status of a row.
  virtual MPSolver::BasisStatus row_status(int constraint_index) const;
  // Returns the basis status of a column.
  virtual MPSolver::BasisStatus column_status(int variable_index) const;

  // Checks whether a feasible solution exists.
  virtual void CheckSolutionExists() const;
  // Checks whether information on the best objective bound exists.
  virtual void CheckBestObjectiveBoundExists() const;

  // ----- Misc -----
  // Write model
  virtual void WriteModel(const string& filename);

  // Query problem type.
  virtual bool IsContinuous() const { return IsLP(); }
  virtual bool IsLP() const { return !mip_; }
  virtual bool IsMIP() const { return mip_; }

  virtual void ExtractNewVariables();
  virtual void ExtractNewConstraints();
  virtual void ExtractObjective();

  virtual string SolverVersion() const {
    return StringPrintf("GLPK %s", glp_version());
  }

  virtual void* underlying_solver() {
    return reinterpret_cast<void*>(lp_);
  }

  virtual double ComputeExactConditionNumber() const;

 private:
  // Configure the solver's parameters.
  void ConfigureGLPKParameters(const MPSolverParameters& param);

  // Set all parameters in the underlying solver.
  virtual void SetParameters(const MPSolverParameters& param);
  // Set each parameter in the underlying solver.
  virtual void SetRelativeMipGap(double value);
  virtual void SetPrimalTolerance(double value);
  virtual void SetDualTolerance(double value);
  virtual void SetPresolveMode(int value);
  virtual void SetLpAlgorithm(int value);

  void ExtractOldConstraints();
  void ExtractOneConstraint(MPConstraint* const constraint,
                            int* const indices,
                            double* const coefs);
  // Transforms basis status from GLPK integer code to MPSolver::BasisStatus.
  MPSolver::BasisStatus TransformGLPKBasisStatus(int glpk_basis_status) const;

  // Computes the L1-norm of the current scaled basis.
  // The L1-norm |A| is defined as max_j sum_i |a_ij|
  // This method is available only for continuous problems.
  double ComputeScaledBasisL1Norm(
    int num_rows, int num_cols,
    double* row_scaling_factor, double* column_scaling_factor) const;

  // Computes the L1-norm of the inverse of the current scaled
  // basis.
  // This method is available only for continuous problems.
  double ComputeInverseScaledBasisL1Norm(
    int num_rows, int num_cols,
    double* row_scaling_factor, double* column_scaling_factor) const;

  glp_prob* lp_;
  bool mip_;

  // Parameters
  glp_smcp lp_param_;
  glp_iocp mip_param_;
  // For the callback
  scoped_ptr<GLPKInformation> mip_callback_info_;
};

// Creates a LP/MIP instance with the specified name and minimization objective.
GLPKInterface::GLPKInterface(MPSolver* const solver, bool mip)
    : MPSolverInterface(solver), lp_(NULL), mip_(mip),
      mip_callback_info_(NULL) {
  lp_ = glp_create_prob();
  glp_set_prob_name(lp_, solver_->name_.c_str());
  glp_set_obj_dir(lp_, GLP_MIN);
  mip_callback_info_.reset(new GLPKInformation(maximize_));
}

// Frees the LP memory allocations.
GLPKInterface::~GLPKInterface() {
  CHECK_NOTNULL(lp_);
  glp_delete_prob(lp_);
  lp_ = NULL;
}

void GLPKInterface::Reset() {
  CHECK_NOTNULL(lp_);
  glp_delete_prob(lp_);
  lp_ = glp_create_prob();
  glp_set_prob_name(lp_, solver_->name_.c_str());
  glp_set_obj_dir(lp_, maximize_ ? GLP_MAX : GLP_MIN);
  ResetExtractionInformation();
}

void GLPKInterface::WriteModel(const string& filename) {
  if (HasSuffixString(filename, ".lp")) {
    glp_write_lp(lp_, NULL, filename.c_str());
  } else {
    glp_write_mps(lp_, GLP_MPS_FILE, NULL, filename.c_str());
  }
}

// ------ Model modifications and extraction -----

// Not cached
void GLPKInterface::SetOptimizationDirection(bool maximize) {
  InvalidateSolutionSynchronization();
  glp_set_obj_dir(lp_, maximize ? GLP_MAX : GLP_MIN);
}

void GLPKInterface::SetVariableBounds(int var_index, double lb, double ub) {
  InvalidateSolutionSynchronization();
  if (var_index != kNoIndex) {
    // Not cached if the variable has been extracted.
    DCHECK(lp_ != NULL);
    const double infinity = solver_->infinity();
    if (lb != -infinity) {
      if (ub != infinity) {
        if (lb == ub) {
          glp_set_col_bnds(lp_, var_index, GLP_FX, lb, ub);
        } else {
          glp_set_col_bnds(lp_, var_index, GLP_DB, lb, ub);
        }
      } else {
        glp_set_col_bnds(lp_, var_index, GLP_LO, lb, 0.0);
      }
    } else if (ub != infinity) {
      glp_set_col_bnds(lp_, var_index, GLP_UP, 0.0, ub);
    } else {
      glp_set_col_bnds(lp_, var_index, GLP_FR, 0.0, 0.0);
    }
  } else {
    sync_status_ = MUST_RELOAD;
  }
}

void GLPKInterface::SetVariableInteger(int var_index, bool integer) {
  InvalidateSolutionSynchronization();
  if (mip_) {
    if (var_index != kNoIndex) {
      // Not cached if the variable has been extracted.
     glp_set_col_kind(lp_, var_index, integer ? GLP_IV : GLP_CV);
    } else {
      sync_status_ = MUST_RELOAD;
    }
  }
}

void GLPKInterface::SetConstraintBounds(int index, double lb, double ub) {
  InvalidateSolutionSynchronization();
  if (index != kNoIndex) {
    // Not cached if the row has been extracted
    DCHECK(lp_ != NULL);
    const double infinity = solver_->infinity();
    if (lb != -infinity) {
      if (ub != infinity) {
        if (lb == ub) {
          glp_set_row_bnds(lp_, index, GLP_FX, lb, ub);
        } else {
          glp_set_row_bnds(lp_, index, GLP_DB, lb, ub);
        }
      } else {
        glp_set_row_bnds(lp_, index, GLP_LO, lb, 0.0);
      }
    } else if (ub != infinity) {
      glp_set_row_bnds(lp_, index, GLP_UP, 0.0, ub);
    } else {
      glp_set_row_bnds(lp_, index, GLP_FR, 0.0, 0.0);
    }
  } else {
    sync_status_ = MUST_RELOAD;
  }
}

void GLPKInterface::SetCoefficient(MPConstraint* const constraint,
                                   const MPVariable* const variable,
                                   double new_value,
                                   double old_value) {
  InvalidateSolutionSynchronization();
  // GLPK does not allow to modify one coefficient at a time, so we
  // extract the whole constraint again, if it has been extracted
  // already and if it does not contain new variables. Otherwise, we
  // cache the modification.
  if (constraint->index() != kNoIndex &&
      (sync_status_ == MODEL_SYNCHRONIZED ||
       !constraint->ContainsNewVariables())) {
    const int size = constraint->coefficients_.size();
    scoped_array<int> indices(new int[size + 1]);
    scoped_array<double> coefs(new double[size + 1]);
    ExtractOneConstraint(constraint, indices.get(), coefs.get());
  }
}

// Not cached
void GLPKInterface::ClearConstraint(MPConstraint* const constraint) {
  InvalidateSolutionSynchronization();
  const int constraint_index = constraint->index();
  // Constraint may have not been extracted yet.
  if (constraint_index != kNoIndex) {
    glp_set_mat_row(lp_, constraint_index, 0, NULL, NULL);
  }
}

// Cached
void GLPKInterface::SetObjectiveCoefficient(const MPVariable* const variable,
                                            double coefficient) {
  sync_status_ = MUST_RELOAD;
}

// Cached
void GLPKInterface::SetObjectiveOffset(double value) {
  sync_status_ = MUST_RELOAD;
}

// Clear objective of all its terms (linear)
void GLPKInterface::ClearObjective() {
  InvalidateSolutionSynchronization();
  for (ConstIter<hash_map<const MPVariable*, double> > it(
           solver_->objective_->coefficients_);
       !it.at_end(); ++it) {
    const int var_index = it->first->index();
    // Variable may have not been extracted yet.
    if (var_index == kNoIndex) {
      DCHECK_NE(MODEL_SYNCHRONIZED, sync_status_);
    } else {
      glp_set_obj_coef(lp_, var_index, 0.0);
    }
  }
  // Constant term.
  glp_set_obj_coef(lp_, 0, 0.0);
}

void GLPKInterface::AddRowConstraint(MPConstraint* const ct) {
  sync_status_ = MUST_RELOAD;
}

void GLPKInterface::AddVariable(MPVariable* const var) {
  sync_status_ = MUST_RELOAD;
}

// Define new variables and add them to existing constraints.
void GLPKInterface::ExtractNewVariables() {
  int total_num_vars = solver_->variables_.size();
  if (total_num_vars > last_variable_index_) {
    glp_add_cols(lp_, total_num_vars - last_variable_index_);
    for (int j = last_variable_index_; j < solver_->variables_.size(); ++j) {
      MPVariable* const var = solver_->variables_[j];
      // GLPK convention is to start indexing at 1.
      const int var_index = j + 1;
      var->set_index(var_index);
      if (!var->name().empty()) {
        glp_set_col_name(lp_, var_index, var->name().c_str());
      }
      SetVariableBounds(var->index(), var->lb(), var->ub());
      SetVariableInteger(var->index(), var->integer());

      // The true objective coefficient will be set later in ExtractObjective.
      double tmp_obj_coef = 0.0;
      glp_set_obj_coef(lp_, var->index(), tmp_obj_coef);
    }
    // Add new variables to the existing constraints.
    ExtractOldConstraints();
  }
}

// Extract again existing constraints if they contain new variables.
void GLPKInterface::ExtractOldConstraints() {
  int max_constraint_size = solver_->ComputeMaxConstraintSize(
      0, last_constraint_index_);
  // The first entry in the following arrays is dummy, to be
  // consistent with glpk API.
  scoped_array<int> indices(new int[max_constraint_size + 1]);
  scoped_array<double> coefs(new double[max_constraint_size + 1]);

  for (int i = 0; i < last_constraint_index_; ++i) {
    MPConstraint* const  ct = solver_->constraints_[i];
    DCHECK_NE(kNoIndex, ct->index());
    const int size = ct->coefficients_.size();
    if (size == 0) {
      continue;
    }
    // Update the constraint's coefficients if it contains new variables.
    if (ct->ContainsNewVariables()) {
      ExtractOneConstraint(ct, indices.get(), coefs.get());
    }
  }
}

// Extract one constraint. Arrays indices and coefs must be
// preallocated to have enough space to contain the constraint's
// coefficients.
void GLPKInterface::ExtractOneConstraint(MPConstraint* const constraint,
                                         int* const indices,
                                         double* const coefs) {
  // GLPK convention is to start indexing at 1.
  int k = 1;
  for (ConstIter<hash_map<const MPVariable*, double> > it(
           constraint->coefficients_);
       !it.at_end(); ++it) {
    const int var_index = it->first->index();
    DCHECK_NE(kNoIndex, var_index);
    indices[k] = var_index;
    coefs[k] = it->second;
    ++k;
  }
  glp_set_mat_row(lp_, constraint->index(), k-1, indices, coefs);
}

// Define new constraints on old and new variables.
void GLPKInterface::ExtractNewConstraints() {
  int total_num_rows = solver_->constraints_.size();
  if (last_constraint_index_ < total_num_rows) {
    // Define new constraints
    glp_add_rows(lp_, total_num_rows - last_constraint_index_);
    int num_coefs = 0;
    for (int i = last_constraint_index_; i < total_num_rows; ++i) {
      // GLPK convention is to start indexing at 1.
      const int constraint_index = i + 1;
      MPConstraint* ct = solver_->constraints_[i];
      ct->set_index(constraint_index);
      if (ct->name().empty()) {
        glp_set_row_name(lp_, constraint_index,
                         StringPrintf("ct_%i", i).c_str());
      } else {
        glp_set_row_name(lp_, constraint_index, ct->name().c_str());
      }
      // All constraints are set to be of the type <= limit_ .
      SetConstraintBounds(constraint_index, ct->lb(), ct->ub());
      num_coefs += ct->coefficients_.size();
    }

    // Fill new constraints with coefficients
    if (last_variable_index_ == 0 && last_constraint_index_ == 0) {
      // Faster extraction when nothing has been extracted yet: build
      // and load whole matrix at once instead of constructing rows
      // separately.

      // The first entry in the following arrays is dummy, to be
      // consistent with glpk API.
      scoped_array<int> variable_indices(new int[num_coefs + 1]);
      scoped_array<int> constraint_indices(new int[num_coefs + 1]);
      scoped_array<double> coefs(new double[num_coefs + 1]);
      int k = 1;
      for (int i = 0; i < solver_->constraints_.size(); ++i) {
        MPConstraint* ct = solver_->constraints_[i];
        for (hash_map<const MPVariable*, double>::const_iterator it =
                 ct->coefficients_.begin();
             it != ct->coefficients_.end();
             ++it) {
          DCHECK_NE(kNoIndex, it->first->index());
          constraint_indices[k] = ct->index();
          variable_indices[k] = it->first->index();
          coefs[k] = it->second;
          ++k;
        }
      }
      CHECK_EQ(num_coefs + 1, k);
      glp_load_matrix(lp_, num_coefs, constraint_indices.get(),
                      variable_indices.get(), coefs.get());
    } else {
      // Build each new row separately.
      int max_constraint_size = solver_->ComputeMaxConstraintSize(
          last_constraint_index_, total_num_rows);
      // The first entry in the following arrays is dummy, to be
      // consistent with glpk API.
      scoped_array<int> indices(new int[max_constraint_size + 1]);
      scoped_array<double> coefs(new double[max_constraint_size + 1]);
      for (int i = last_constraint_index_; i < total_num_rows; i++) {
        ExtractOneConstraint(solver_->constraints_[i], indices.get(),
                             coefs.get());
      }
    }
  }
}

void GLPKInterface::ExtractObjective() {
  // Linear objective: set objective coefficients for all variables
  // (some might have been modified).
  for (hash_map<const MPVariable*, double>::const_iterator it =
           solver_->objective_->coefficients_.begin();
       it != solver_->objective_->coefficients_.end();
       ++it) {
    glp_set_obj_coef(lp_, it->first->index(), it->second);
  }
  // Constant term.
  glp_set_obj_coef(lp_, 0, solver_->Objective().offset());
}

// Solve the problem using the parameter values specified.
MPSolver::ResultStatus GLPKInterface::Solve(const MPSolverParameters& param) {
  WallTimer timer;
  timer.Start();

  // Note that GLPK provides incrementality for LP but not for MIP.
  if (param.GetIntegerParam(MPSolverParameters::INCREMENTALITY) ==
      MPSolverParameters::INCREMENTALITY_OFF) {
    Reset();
  }

  // Set log level.
  if (quiet_) {
    glp_term_out(GLP_OFF);
  } else {
    glp_term_out(GLP_ON);
  }

  ExtractModel();
  VLOG(1) << StringPrintf("Model built in %.3f seconds.", timer.Get());

  WriteModelToPredefinedFiles();

  // Configure parameters at every solve, even when the model has not
  // been changed, in case some of the parameters such as the time
  // limit have been changed since the last solve.
  ConfigureGLPKParameters(param);

  // Solve
  timer.Restart();
  if (mip_) {
    // glp_intopt requires to solve the root LP separately.
    int simplex_status = glp_simplex(lp_, &lp_param_);
    // If the root LP was solved successfully, solve the MIP.
    if (simplex_status == 0) {
      glp_intopt(lp_, &mip_param_);
    } else {
      // Something abnormal occurred during the root LP solve. It is
      // highly unlikely that an integer feasible solution is
      // available at this point, so we don't put any effort in trying
      // to recover it.
      result_status_ = MPSolver::ABNORMAL;
      sync_status_ = SOLUTION_SYNCHRONIZED;
      return result_status_;
    }
  } else {
    glp_simplex(lp_, &lp_param_);
  }
  VLOG(1) << StringPrintf("Solved in %.3f seconds.", timer.Get());

  // Get the results.
  if (mip_) {
    objective_value_ = glp_mip_obj_val(lp_);
  } else {
    objective_value_ = glp_get_obj_val(lp_);
  }
  VLOG(1) << "objective=" << objective_value_;
  for (int i = 0; i < solver_->variables_.size(); ++i) {
    MPVariable* const var = solver_->variables_[i];
    double val;
    if (mip_) {
      val = glp_mip_col_val(lp_, var->index());
    } else {
      val = glp_get_col_prim(lp_, var->index());
    }
    var->set_solution_value(val);
    VLOG(3) << var->name() << ": value =" << val;
    if (!mip_) {
      double reduced_cost;
      reduced_cost = glp_get_col_dual(lp_, var->index());
      var->set_reduced_cost(reduced_cost);
      VLOG(4) << var->name() << ": reduced cost = " << reduced_cost;
    }
  }
  for (int i = 0; i < solver_->constraints_.size(); ++i) {
    MPConstraint* const ct = solver_->constraints_[i];
    if (mip_) {
      const double row_activity = glp_mip_row_val(lp_, ct->index());
      ct->set_activity(row_activity);
      VLOG(4) << "row " << ct->index()
              << ": activity = " << row_activity;
    } else {
      const double row_activity = glp_get_row_prim(lp_, ct->index());
      ct->set_activity(row_activity);
      const double dual_value = glp_get_row_dual(lp_, ct->index());
      ct->set_dual_value(dual_value);
      VLOG(4) << "row " << ct->index()
              << ": activity = " << row_activity
              << ": dual value = " << dual_value;
    }
  }

  // Check the status: optimal, infeasible, etc.
  if (mip_) {
    int tmp_status = glp_mip_status(lp_);
    VLOG(1) << "gplk result status: " << tmp_status;
    if (tmp_status == GLP_OPT) {
      result_status_ = MPSolver::OPTIMAL;
    } else if (tmp_status == GLP_FEAS) {
      result_status_ = MPSolver::FEASIBLE;
    } else if (tmp_status == GLP_NOFEAS) {
      // For infeasible problems, GLPK actually seems to return
      // GLP_UNDEF. So this is never (?) reached.  Return infeasible
      // in case GLPK returns a correct status in future versions.
      result_status_ = MPSolver::INFEASIBLE;
    } else {
      result_status_ = MPSolver::ABNORMAL;
      // GLPK does not have a status code for unbounded MIP models, so
      // we return an abnormal status instead.
    }
  } else {
    int tmp_status = glp_get_status(lp_);
    VLOG(1) << "gplk result status: " << tmp_status;
    if (tmp_status == GLP_OPT) {
      result_status_ = MPSolver::OPTIMAL;
    } else if (tmp_status == GLP_FEAS) {
      result_status_ = MPSolver::FEASIBLE;
    } else if (tmp_status == GLP_NOFEAS ||
               tmp_status == GLP_INFEAS) {
      // For infeasible problems, GLPK actually seems to return
      // GLP_UNDEF. So this is never (?) reached.  Return infeasible
      // in case GLPK returns a correct status in future versions.
      result_status_ = MPSolver::INFEASIBLE;
    } else if (tmp_status == GLP_UNBND) {
      // For unbounded problems, GLPK actually seems to return
      // GLP_UNDEF. So this is never (?) reached.  Return unbounded
      // in case GLPK returns a correct status in future versions.
      result_status_ = MPSolver::UNBOUNDED;
    } else {
      result_status_ = MPSolver::ABNORMAL;
    }
  }

  sync_status_ = SOLUTION_SYNCHRONIZED;

  return result_status_;
}

MPSolver::BasisStatus
GLPKInterface::TransformGLPKBasisStatus(int glpk_basis_status) const {
  switch (glpk_basis_status) {
    case GLP_BS:
      return MPSolver::BASIC;
    case GLP_NL:
      return MPSolver::AT_LOWER_BOUND;
    case GLP_NU:
      return MPSolver::AT_UPPER_BOUND;
    case GLP_NF:
      return MPSolver::FREE;
    case GLP_NS:
      return MPSolver::FIXED_VALUE;
    default:
      LOG(FATAL) << "Unknown GLPK basis status";
      return MPSolver::FREE;
  }
}

MPSolverInterface* BuildGLPKInterface(MPSolver* const solver, bool mip) {
  return new GLPKInterface(solver, mip);
}

// ------ Query statistics on the solution and the solve ------

int64 GLPKInterface::iterations() const {
  if (mip_) {
    LOG(WARNING) << "Total number of iterations is not available";
    return kUnknownNumberOfIterations;
  } else {
    CheckSolutionIsSynchronized();
    return lpx_get_int_parm(lp_, LPX_K_ITCNT);
  }
}

int64 GLPKInterface::nodes() const {
  if (mip_) {
    CheckSolutionIsSynchronized();
    return mip_callback_info_->num_all_nodes_;
  } else {
    LOG(FATAL) << "Number of nodes only available for discrete problems";
    return kUnknownNumberOfNodes;
  }
}

double GLPKInterface::best_objective_bound() const {
  if (mip_) {
    CheckSolutionIsSynchronized();
    CheckBestObjectiveBoundExists();
    if (solver_->variables_.size() == 0 && solver_->constraints_.size() == 0) {
      // Special case for empty model.
      return solver_->Objective().offset();
    } else {
      return mip_callback_info_->best_objective_bound_;
    }
  } else {
    LOG(FATAL) << "Best objective bound only available for discrete problems";
    return 0.0;
  }
}

MPSolver::BasisStatus GLPKInterface::row_status(int constraint_index) const {
  // + 1 because of GLPK indexing convention.
  DCHECK_LE(1, constraint_index);
  DCHECK_GT(last_constraint_index_ + 1, constraint_index);
  const int glpk_basis_status = glp_get_row_stat(lp_, constraint_index);
  return TransformGLPKBasisStatus(glpk_basis_status);
}

MPSolver::BasisStatus GLPKInterface::column_status(int variable_index) const {
  // + 1 because of GLPK indexing convention.
  DCHECK_LE(1, variable_index);
  DCHECK_GT(last_variable_index_ + 1, variable_index);
  const int glpk_basis_status = glp_get_col_stat(lp_, variable_index);
  return TransformGLPKBasisStatus(glpk_basis_status);
}


void GLPKInterface::CheckSolutionExists() const {
  if (result_status_ == MPSolver::ABNORMAL) {
    LOG(WARNING) << "Ignoring ABNORMAL status from GLPK: This status may or may"
                 << " not indicate that a solution exists.";
  } else {
    // Call default implementation
    MPSolverInterface::CheckSolutionExists();
  }
}

void GLPKInterface::CheckBestObjectiveBoundExists() const {
  if (result_status_ == MPSolver::ABNORMAL) {
    LOG(WARNING) << "Ignoring ABNORMAL status from GLPK: This status may or may"
                 << " not indicate that information is available on the best"
                 << " objective bound.";
  } else {
    // Call default implementation
    MPSolverInterface::CheckBestObjectiveBoundExists();
  }
}

double GLPKInterface::ComputeExactConditionNumber() const {
  CHECK(IsContinuous()) <<
      "Condition number only available for continuous problems";
  CheckSolutionIsSynchronized();
  // Simplex is the only LP algorithm supported in the wrapper for
  // GLPK, so when a solution exists, a basis exists.
  CheckSolutionExists();
  const int num_rows = glp_get_num_rows(lp_);
  const int num_cols = glp_get_num_cols(lp_);
  // GLPK indexes everything starting from 1 instead of 0.
  scoped_array<double> row_scaling_factor(new double[num_rows + 1]);
  scoped_array<double> column_scaling_factor(new double[num_cols + 1]);
  for (int row = 1; row <= num_rows; ++row) {
    row_scaling_factor[row] = glp_get_rii(lp_, row);
  }
  for (int col = 1; col <= num_cols; ++col) {
    column_scaling_factor[col] = glp_get_sjj(lp_, col);
  }
  return
      ComputeInverseScaledBasisL1Norm(
          num_rows, num_cols,
          row_scaling_factor.get(), column_scaling_factor.get()) *
      ComputeScaledBasisL1Norm(
          num_rows, num_cols,
          row_scaling_factor.get(), column_scaling_factor.get());
}

double GLPKInterface::ComputeScaledBasisL1Norm(
    int num_rows, int num_cols,
    double* row_scaling_factor, double* column_scaling_factor) const {
  double norm = 0.0;
  scoped_array<double> values(new double[num_rows + 1]);
  scoped_array<int> indices(new int[num_rows + 1]);
  for (int col = 1; col <= num_cols; ++col) {
    const int glpk_basis_status = glp_get_col_stat(lp_, col);
    // Take into account only basic columns.
    if (glpk_basis_status == GLP_BS) {
      // Compute L1-norm of column 'col': sum_row |a_row,col|.
      const int num_nz = glp_get_mat_col(lp_, col, indices.get(), values.get());
      double column_norm = 0.0;
      for (int k = 1; k <= num_nz; k++) {
        column_norm += fabs(values[k] * row_scaling_factor[indices[k]]);
      }
      column_norm *= fabs(column_scaling_factor[col]);
      // Compute max_col column_norm
      norm = std::max(norm, column_norm);
    }
  }
  // Slack variables.
  for (int row = 1; row <= num_rows; ++row) {
    const int glpk_basis_status = glp_get_row_stat(lp_, row);
    // Take into account only basic slack variables.
    if (glpk_basis_status == GLP_BS) {
      // Only one non-zero coefficient: +/- 1.0 in the corresponding
      // row. The row has a scaling coefficient but the slack variable
      // is never scaled on top of that.
      const double column_norm = fabs(row_scaling_factor[row]);
      // Compute max_col column_norm
      norm = std::max(norm, column_norm);
    }
  }
  return norm;
}

double GLPKInterface::ComputeInverseScaledBasisL1Norm(
    int num_rows, int num_cols,
    double* row_scaling_factor, double* column_scaling_factor) const {
  // Compute the LU factorization if it doesn't exist yet.
  if (!glp_bf_exists(lp_)) {
    const int factorize_status = glp_factorize(lp_);
    switch (factorize_status) {
      case GLP_EBADB: {
        LOG(FATAL) << "Not able to factorize: error GLP_EBADB.";
        break;
      }
      case GLP_ESING: {
        LOG(WARNING)
            << "Not able to factorize: "
            << "the basis matrix is singular within the working precision.";
        return MPSolver::infinity();
      }
      case GLP_ECOND: {
        LOG(WARNING)
            << "Not able to factorize: the basis matrix is ill-conditioned.";
        return MPSolver::infinity();
      }
      default:
        break;
    }
  }
  scoped_array<double> right_hand_side(new double[num_rows + 1]);
  double norm = 0.0;
  // Iteratively solve B x = e_k, where e_k is the kth unit vector.
  // The result of this computation is the kth column of B^-1.
  // glp_ftran works on original matrix. Scale input and result to
  // obtain the norm of the kth column in the inverse scaled
  // matrix. See glp_ftran documentation in glpapi12.c for how the
  // scaling is done: inv(B'') = inv(SB) * inv(B) * inv(R) where:
  // o B'' is the scaled basis
  // o B is the original basis
  // o R is the diagonal row scaling matrix
  // o SB consists of the basic columns of the augmented column
  // scaling matrix (auxiliary variables then structural variables):
  // S~ = diag(inv(R) | S).
  for (int k = 1; k <= num_rows; ++k) {
    for (int row = 1; row <= num_rows; ++row) {
      right_hand_side[row] = 0.0;
    }
    right_hand_side[k] = 1.0;
    // Multiply input by inv(R).
    for (int row = 1; row <= num_rows; ++row) {
      right_hand_side[row] /= row_scaling_factor[row];
    }
    glp_ftran(lp_, right_hand_side.get());
    // glp_ftran stores the result in the same vector where the right
    // hand side was provided.
    // Multiply result by inv(SB).
    for (int row = 1; row <= num_rows; ++row) {
      const int k = glp_get_bhead(lp_, row);
      if (k <= num_rows) {
        // Auxiliary variable.
        right_hand_side[row] *= row_scaling_factor[k];
      } else {
        // Structural variable.
        right_hand_side[row] /= column_scaling_factor[k - num_rows];
      }
    }
    // Compute sum_row |vector_row|.
    double column_norm = 0.0;
    for (int row = 1; row <= num_rows; ++row) {
      column_norm += fabs(right_hand_side[row]);
    }
    // Compute max_col column_norm
    norm = std::max(norm, column_norm);
  }
  return norm;
}

// ------ Parameters ------

void GLPKInterface::ConfigureGLPKParameters(const MPSolverParameters& param) {
  if (mip_) {
    glp_init_iocp(&mip_param_);
    // Time limit
    if (solver_->time_limit()) {
      VLOG(1) << "Setting time limit = " << solver_->time_limit() << " ms.";
      mip_param_.tm_lim = solver_->time_limit();
    }
    // Initialize structures related to the callback.
    mip_param_.cb_func = GLPKGatherInformationCallback;
    mip_callback_info_->Reset(maximize_);
    mip_param_.cb_info = mip_callback_info_.get();
    // TODO(user): switch some cuts on? All cuts are off by default!?
  }

  // Configure LP parameters in all cases since they will be used to
  // solve the root LP in the MIP case.
  glp_init_smcp(&lp_param_);
  // Time limit
  if (solver_->time_limit()) {
    VLOG(1) << "Setting time limit = " << solver_->time_limit() << " ms.";
    lp_param_.tm_lim = solver_->time_limit();
  }

  // Should give a numerically better representation of the problem.
  glp_scale_prob(lp_, GLP_SF_AUTO);

  // Use advanced initial basis (options: standard / advanced / Bixby's).
  glp_adv_basis(lp_, NULL);

  // Set parameters specified by the user.
  SetParameters(param);
}

void GLPKInterface::SetParameters(const MPSolverParameters& param) {
  SetCommonParameters(param);
  if (mip_) {
    SetMIPParameters(param);
  }
}

void GLPKInterface::SetRelativeMipGap(double value) {
  if (mip_) {
    mip_param_.mip_gap = value;
  } else {
    LOG(WARNING) << "The relative MIP gap is only available "
                 << "for discrete problems.";
  }
}

void GLPKInterface::SetPrimalTolerance(double value) {
  lp_param_.tol_bnd = value;
}

void GLPKInterface::SetDualTolerance(double value) {
  lp_param_.tol_dj = value;
}

void GLPKInterface::SetPresolveMode(int value) {
  switch (value) {
    case MPSolverParameters::PRESOLVE_OFF: {
      mip_param_.presolve = GLP_OFF;
      lp_param_.presolve = GLP_OFF;
      break;
    }
    case MPSolverParameters::PRESOLVE_ON: {
      mip_param_.presolve = GLP_ON;
      lp_param_.presolve = GLP_ON;
      break;
    }
    default: {
      SetIntegerParamToUnsupportedValue(MPSolverParameters::PRESOLVE, value);
    }
  }
}

void GLPKInterface::SetLpAlgorithm(int value) {
  switch (value) {
    case MPSolverParameters::DUAL: {
      // Use dual, and if it fails, switch to primal.
      lp_param_.meth = GLP_DUALP;
      break;
    }
    case MPSolverParameters::PRIMAL: {
      lp_param_.meth = GLP_PRIMAL;
      break;
    }
    case MPSolverParameters::BARRIER:
    default: {
      SetIntegerParamToUnsupportedValue(MPSolverParameters::LP_ALGORITHM,
                                        value);
    }
  }
}

}  // namespace operations_research
#endif  //  #if defined(USE_GLPK)

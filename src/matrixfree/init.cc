//init() method for MatrixFreePDE class

#ifndef INIT_MATRIXFREE_H
#define INIT_MATRIXFREE_H
//this source file is temporarily treated as a header file (hence
//#ifndef's) till library packaging scheme is finalized

 //populate with fields and setup matrix free system
 template <int dim>
 void MatrixFreePDE<dim>::init(unsigned int iter){
   //void MatrixFreePDE<dim>::init(std::vector<Field<dim> >& _fields){
   computing_timer.enter_section("matrixFreePDE: initialization"); 

   //
   if (iter==0){
     //creating mesh
     std::vector<unsigned int> subdivisions;
     subdivisions.push_back(subdivisionsX);
     if (dim>1){
       subdivisions.push_back(subdivisionsY);
       if (dim>2){
	 subdivisions.push_back(subdivisionsZ);
       }
     }
     
     pcout << "creating problem mesh...\n";
#if problemDIM==3
     GridGenerator::subdivided_hyper_rectangle (triangulation, subdivisions, Point<dim>(), Point<dim>(spanX,spanY,spanZ));
#elif problemDIM==2
     GridGenerator::subdivided_hyper_rectangle (triangulation, subdivisions, Point<dim>(), Point<dim>(spanX,spanY));
#elif problemDIM==1
     GridGenerator::subdivided_hyper_rectangle (triangulation, subdivisions, Point<dim>(), Point<dim>(spanX));
#endif
     triangulation.refine_global (refineFactor);
     //write out extends
     pcout << "problem dimensions: " << spanX << "x" << spanY << "x" << spanZ << std::endl;
     pcout << "number of elements: " << triangulation.n_global_active_cells() << std::endl;
     pcout << std::endl;
  
     //mark boundaries for applying Dirichlet boundary conditons
     markBoundaries();
   }
   else{
     refineGrid();
   }
     
   //setup system
   pcout << "initializing matrix free object\n";
   unsigned int totalDOFs=0;
   for(typename std::vector<Field<dim> >::iterator it = fields.begin(); it != fields.end(); ++it){
     char buffer[100];
     if (iter==0){
       //print to std::out
       sprintf(buffer,"initializing finite element space P^%u for %9s:%6s field '%s'\n", \
	       finiteElementDegree,					\
	       (it->pdetype==PARABOLIC ? "PARABOLIC":"ELLIPTIC"),	\
	       (it->type==SCALAR ? "SCALAR":"VECTOR"),			\
	       it->name.c_str());
       pcout << buffer;
       //check if any time dependent fields present
       if (it->pdetype==PARABOLIC){
	 isTimeDependentBVP=true;
       }
     }

     
     //create FESystem
     FESystem<dim>* fe;
     //
     if (iter==0){
       if (it->type==SCALAR){
	 fe=new FESystem<dim>(FE_Q<dim>(QGaussLobatto<1>(finiteElementDegree+1)),1);
       }
       else if (it->type==VECTOR){
	 fe=new FESystem<dim>(FE_Q<dim>(QGaussLobatto<1>(finiteElementDegree+1)),dim);
       }
       else{
	 pcout << "\nmatrixFreePDE.h: unknown field type\n";
	 exit(-1);
       }
       FESet.push_back(fe);
     }
     else{
       fe=FESet.at(it->index);
     }
     
     //distribute DOFs
     DoFHandler<dim>* dof_handler;
     if (iter==0){
       dof_handler=new DoFHandler<dim>(triangulation);
       dofHandlersSet.push_back(dof_handler);
       dofHandlersSet2.push_back(dof_handler); 
     }
     else{
       dof_handler=dofHandlersSet2.at(it->index);
     }
     dof_handler->distribute_dofs (*fe);
     totalDOFs+=dof_handler->n_dofs();

     //extract locally_relevant_dofs
     IndexSet* locally_relevant_dofs;
     if (iter==0){
       locally_relevant_dofs=new IndexSet;
       locally_relevant_dofsSet.push_back(locally_relevant_dofs);
       locally_relevant_dofsSet2.push_back(locally_relevant_dofs);
     }
     else{
       locally_relevant_dofs=locally_relevant_dofsSet2.at(it->index);
     }
     locally_relevant_dofs->clear();
     DoFTools::extract_locally_relevant_dofs (*dof_handler, *locally_relevant_dofs);


     //create constraints
     ConstraintMatrix* constraints;
     if (iter==0){
       constraints=new ConstraintMatrix;
       constraintsSet.push_back(constraints);
       constraintsSet2.push_back(constraints);   
     }
     else{
       constraints=constraintsSet2.at(it->index);
     }
     constraints->clear();
     constraints->reinit(*locally_relevant_dofs);
     DoFTools::make_hanging_node_constraints (*dof_handler, *constraints);

     //apply zero Dirichlet BC's for ELLIPTIC fields. This is just the
     //default and can be changed later in the specific BVP
     //implementation
     if (it->pdetype==ELLIPTIC){
       currentFieldIndex=it->index;
       applyDirichletBCs();
     }
     constraints->close();  
     sprintf(buffer, "field '%2s' DOF : %u (Dirichlet DOF : %u)\n", \
	     it->name.c_str(), dof_handler->n_dofs(), constraints->n_constraints());
     pcout << buffer;
   }
   pcout << "total DOF : " << totalDOFs << std::endl;

   //setup the matrix free object
   typename MatrixFree<dim,double>::AdditionalData additional_data;
   additional_data.mpi_communicator = MPI_COMM_WORLD;
   additional_data.tasks_parallel_scheme = MatrixFree<dim,double>::AdditionalData::partition_partition;
   additional_data.mapping_update_flags = (update_values | update_gradients | update_JxW_values | update_quadrature_points);
   QGaussLobatto<1> quadrature (finiteElementDegree+1);
   num_quadrature_points=std::pow(quadrature.size(),dim);
   matrixFreeObject.clear();
   matrixFreeObject.reinit (dofHandlersSet, constraintsSet, quadrature, additional_data);
 
   //setup problem vectors
   pcout << "initializing parallel::distributed residual and solution vectors\n";
   for(unsigned int fieldIndex=0; fieldIndex<fields.size(); fieldIndex++){
     vectorType *U, *R;
     if (iter==0){
       U=new vectorType; R=new vectorType;
       solutionSet.push_back(U); residualSet.push_back(R);
     }
     else{
       U=solutionSet.at(fieldIndex); R=residualSet.at(fieldIndex);
     }
     matrixFreeObject.initialize_dof_vector(*U,  fieldIndex);
     matrixFreeObject.initialize_dof_vector(*R,  fieldIndex);
     *U=0; *R=0;
     
     //initializing temporary dU vector required for implicit solves of the elliptic equation.
     //Assuming here that there is only one elliptic field in the problem
     if (fields[fieldIndex].pdetype==ELLIPTIC){
    	 matrixFreeObject.initialize_dof_vector(dU,  fieldIndex);
     }
   }
   
   //check if time dependent BVP and compute invM
   if (isTimeDependentBVP){
     computeInvM();
   }

   //apply initial conditions if iter=0, else transfer solution from previous refined mesh
   if (iter==0){
     applyInitialConditions();
   }
   else{
     for(unsigned int fieldIndex=0; fieldIndex<fields.size(); fieldIndex++){
       soltransSet[fieldIndex]->interpolate(*solutionSet[fieldIndex]);
       //clear old solution transfer sets
       delete soltransSet[fieldIndex];
     }
   }

   //create new solution transfer sets
   soltransSet.clear();
   for(unsigned int fieldIndex=0; fieldIndex<fields.size(); fieldIndex++){
     soltransSet.push_back(new parallel::distributed::SolutionTransfer<dim, vectorType>(*dofHandlersSet2[fieldIndex]));
   }
   
   //Ghost the solution vectors. Also apply the Dirichet BC's (if any) on the solution vectors 
   for(unsigned int fieldIndex=0; fieldIndex<fields.size(); fieldIndex++){
     constraintsSet[fieldIndex]->distribute(*solutionSet[fieldIndex]);
     solutionSet[fieldIndex]->update_ghost_values();
   } 

   computing_timer.exit_section("matrixFreePDE: initialization");  
}

template <int dim>
void MatrixFreePDE<dim>::refineGrid (){
  Vector<float> estimated_error_per_cell (triangulation.n_active_cells());
  KellyErrorEstimator<dim>::estimate (*dofHandlersSet2[1],
				      QGauss<dim-1>(finiteElementDegree+2),
				      typename FunctionMap<dim>::type(),
				      *solutionSet[1],
				      estimated_error_per_cell);
  parallel::distributed::GridRefinement::refine_and_coarsen_fixed_fraction (triangulation,
									    estimated_error_per_cell,
									    0.3, 0.1);
  triangulation.prepare_coarsening_and_refinement();
  for(unsigned int fieldIndex=0; fieldIndex<fields.size(); fieldIndex++){
    soltransSet[fieldIndex]->prepare_for_coarsening_and_refinement(*solutionSet[fieldIndex]);
  }
  triangulation.execute_coarsening_and_refinement();
}

#endif 

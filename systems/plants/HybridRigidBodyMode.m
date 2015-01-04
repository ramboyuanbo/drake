classdef HybridRigidBodyMode < DrakeSystem
% Simple shell around an RBM which adds one extra variable which emulates
% the "mode" (but avoids the pitfalls of having many modes as individual
% systems/transitions in the HybridDrakeSystem

  methods
    function obj = HybridRigidBodyMode(model,options)
      if (nargin<2) options=struct(); end
      w = warning('off','Drake:RigidBodyManipulator:UnsupportedContactPoints');
      if (nargin<1)
        [filename,pathname]=uigetfile('*.urdf');
        model = RigidBodyManipulator(fullfile(pathname,filename),options);
      elseif ischar(model)
        model = RigidBodyManipulator(model,options);
      elseif ~isa(model,'RigidBodyManipulator')
        error('model must be a RigidBodyManipulator or the name of a urdf file'); 
      end
      warning(w);
      
      obj = obj@DrakeSystem(getNumStates(model),0,getNumInputs(model),getNumOutputs(model),isDirectFeedthrough(model),isTI(model));
      obj.model = model;  
      obj = setInputFrame(obj,getInputFrame(model));
      obj = setOutputFrame(obj,getOutputFrame(model));
      
      num_phi = 0;
      obj.constraint_ids = [model.joint_limit_constraint_id,model.position_constraint_ids];
      for id=obj.constraint_ids
        num_phi = numel(model.state_constraints{id}.lb);
      end
      
      if model.num_velocity_constraints>0,
        error('velocity constraints not implemented yet (but will be easy)');
      end
      
      obj = setNumDiscStates(obj,num_phi);
      
      obj = setStateFrame(obj,MultiCoordinateFrame.constructFrame({CoordinateFrame('mode_info',num_phi,'m'),getStateFrame(model)}));
    end

    function ts = getSampleTime(obj)
      ts = [0;0];  % continuous time, and no explicit discrete time update)
    end
    
    function x0 = getInitialState(obj)
      x0 = resolveConstraints(obj,randn(getNumStates(obj),1));
    end
    
    function [x0,success] = resolveConstraints(obj,x0,varargin)
      [x0((obj.num_xd+1):end),success] = resolveConstraints(obj.model,x0((obj.num_xd+1):end),varargin{:});

      x = x0((obj.num_xd+1):end);
      q = x(1:obj.model.num_positions);

      constraint_index = 0;
      for id=obj.constraint_ids
        phi = obj.model.state_constraints{id}.eval(q);
        lb = obj.model.state_constraints{id}.lb;
        ub = obj.model.state_constraints{id}.ub;
        
        x0(constraint_index+(1:numel(lb))) = -(phi<=lb) + (phi>=ub) + 2*(phi<=lb & phi>=ub);
        constraint_index = constraint_index+numel(lb);
      end
    end
    
    function xdn = update(obj,t,x,u)
      % intentionally do nothing
      % note: i've set up the sample times so that we shouldn't actually
      % get here during simulation
      xdn = x(1:obj.num_xd);
      error('got here');
    end
    
    function y = output(obj,t,x,u)
      x = x((obj.num_xd+1):end);
      y = output(obj.model,t,x,u);
    end
    
    function xdot = dynamics(mode_obj,t,x,u)
      constraint_state = x(1:mode_obj.num_xd);
      % -1 is lb active
      % 0 is non-active
      % 1 is ub active
      % 2 is both active
      
      x = x((mode_obj.num_xd+1):end);
      obj = mode_obj.model;
      q = x(1:obj.num_positions);
      v = x(obj.num_positions+1:end);
      qd = vToqdot(obj, q) * v;

      [H,C,B] = manipulatorDynamics(obj,q,v);
      Hinv = inv(H);
      if (obj.num_u>0) tau=B*u - C; else tau=-C; end

      if ~any(constraint_state)  % short-circuit the computation below for the trivial (unconstrained) case
        vdot = Hinv*tau;
        xdot = [vToqdot(obj,q)*v; vdot];
        return;
      end
      
      % solve for constraint force with 
      %  min_f ||f||^2 
      %    subject to \phiddot_i >= 0 for all \phi_i <= lb_i
      %    and \phiddot_i <= 0 for all \phi_i >= ub
      %  todo: consider adding stabilization terms back in, too
      %  note: this is not the perfect objective, since the units are
      %  somewhat arbitrary (and dependent on the relative scaling of phi)
      
      nf = 0;

      phi=[]; J=[]; dJ = []; lb=[]; ub=[];
      
      for id=mode_obj.constraint_ids
        [this_phi,this_J, this_dJ] = obj.state_constraints{id}.eval(q);
        lb = [lb; obj.state_constraints{id}.lb];
        ub = [ub; obj.state_constraints{id}.ub];
        phi = [phi;this_phi];
        J = [J; this_J];
        dJ = [dJ; this_dJ];
      end
      
      % todo: find a way to use Jdot*qd directly (ala Twan's code)
      % instead of computing dJ
      Jdotv = dJ*reshape(qd*qd',obj.num_positions^2,1);
      
      % was: lb_inds = phi<=lb; ub_inds = phi>=ub; 
      lb_inds = constraint_state<0.5 | constraint_state>1.5;
      ub_inds = constraint_state>0.5;
      
      nf = numel(phi);
      Jlb = J(lb_inds,:);  Jub = J(ub_inds,:); 
      Ain = [-Jlb*Hinv*J'; Jub*Hinv*J'];
      bin = [Jlb;-Jub]*Hinv*tau + [Jdotv(lb_inds);-Jdotv(ub_inds)];
      
      % todo: use fastqp first?
      prog = QuadraticProgram(eye(nf),zeros(nf,1),Ain,bin);
      f = prog.solve();
      vdot = Hinv*(tau + J'*f);
      
      % useful for debugging
      %if any(lb_inds)
      %  phiddot_lb = Jlb*Hinv*(tau+J'*f)+Jdotv(lb_inds)
      %end
      %if any(ub_inds)
      %  phiddot_ub = Jub*Hinv*(tau+J'*f)+Jdotv(ub_inds)
      %end
      
      xdot = [vToqdot(obj,q)*v; vdot];
    end    
  end
  
  properties
    model
    constraint_ids
  end
end
